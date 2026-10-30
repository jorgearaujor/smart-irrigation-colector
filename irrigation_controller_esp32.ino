/*
  irrigation_controller_esp32.ino

  Porte Arduino IDE (arquivo unico) do firmware de irrigacao. Fonte de
  verdade: firmware/esp32/ (projeto PlatformIO, com testes). Mudancas de
  logica devem ser aplicadas nos dois lugares.

  Placa: ESP32 Dev Module | Framework: Arduino
  Bibliotecas (Arduino Library Manager):
    - DHT sensor library (Adafruit) + Adafruit Unified Sensor
    - LiquidCrystal_I2C (marcoschwartz)
    - ArduinoJson (bblanchon, v7)

  ANTES DE COMPILAR: preencha WIFI_SSID, WIFI_PASSWORD, API_BASE_URL,
  DEVICE_ID e DEVICE_API_KEY abaixo. Recalibre SOIL_MAX_VOLTAGE/
  SOIL_MIN_VOLTAGE apos montar o hardware.

  Contrato HTTP (backend Spring Boot, com.smartirrigation.smartirrigation):
  toda chamada exige o header "X-Device-Key: <DEVICE_API_KEY>".

    POST {API_BASE_URL}/api/webhooks/telemetry
      { deviceId, temperature, humidity, soilPercent, timestamp }
    POST {API_BASE_URL}/api/webhooks/status
      { deviceId, modo, autorizado, quantidadeAgua, tempMin, tempMax, emExecucao, latitude }
    POST {API_BASE_URL}/api/webhooks/history
      { deviceId, umidadeAtual, tempo, volumePorPlanta, volumeTotal, data, hora, modo }
    GET  {API_BASE_URL}/api/devices/{deviceId}/config
      -> 200 com { modo, autorizado, quantidadeAgua, latitude, configIrrigacao }
    GET  {API_BASE_URL}/api/devices/{deviceId}/commands/poll
      -> long polling: 200 + { type, payload } | 204 sem comando
         START_IRRIGATION { quantidade_agua }
         STOP_IRRIGATION  {}
         MODE_CHANGE      { modo }
         AUTHORIZATION_CHANGE { autorizado }
         LATITUDE         { latitude }
         CONFIG_IRRIGACAO { kc, espacamento_plantas_m, espacamento_linhas_m,
                             vazao_gotejador_lph, num_gotejadores_planta,
                             numero_total_plantas, limiar_umidade_minima }

  setup() cria 3 tasks FreeRTOS (sensorTask, statusSyncTask,
  commandPollTask); loop() fica ocioso. O estado compartilhado
  (IrrigationState) e protegido por mutex.
*/

// -------------------- Bibliotecas --------------------
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <esp_adc_cal.h>
#include <time.h>
#include <cmath>
#include <algorithm>
#include <map>

// ==================== Configuracao ====================

// --- Rede e backend HTTP ---
#define WIFI_SSID "VELIKA"
#define WIFI_PASSWORD "velika102025"
#define API_BASE_URL "https://smart-irrigation-api-bhii.onrender.com"
#define DEVICE_ID "esp32-01"
#define DEVICE_API_KEY "7ff55c9e48613340780b303a4b717cf756613d1902068b74f033acc037d30937" // = DEVICE_API_KEY no .env do backend

// --- Pinos de hardware ---
#define RELAY_GPIO_PIN 26
#define DHT_PIN 27
#define DHT_SENSOR_TYPE DHT11
#define SOIL_ADC_PIN 34 // input-only, nao e pino de strapping

// --- Calibracao do sensor de solo (ADC nativo, faixa 0-3.3V) ---
// umidade% = ((SOIL_MAX_VOLTAGE - tensao) / (SOIL_MAX_VOLTAGE - SOIL_MIN_VOLTAGE)) * 100
// MAX = leitura com sensor seco (0%), MIN = leitura com sensor em agua (100%).
#define SOIL_MAX_VOLTAGE 2.800f
#define SOIL_MIN_VOLTAGE 1.200f

// --- LCD ---
#define LCD_ENABLED true
#define LCD_I2C_ADDRESS 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// --- Fuso horario ---
#define TZ_STRING "<-03>3" // America/Sao_Paulo, UTC-3 fixo

// --- Timings (segundos) ---
#define SENSOR_READ_INTERVAL_SECONDS 10
#define STATUS_SYNC_INTERVAL_SECONDS 20
#define DHT_READ_RETRIES 5
#define DHT_READ_RETRY_DELAY_SECONDS 2

// --- HTTP ---
#define API_TIMEOUT_SECONDS 10
#define LONG_POLL_TIMEOUT_SECONDS 30
#define HTTP_MAX_RETRIES 5
#define HTTP_BACKOFF_BASE_SECONDS 2.0f

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ==================== Estado (IrrigationState) ====================

struct ConfigIrrigacao {
  float kc = 1.40f;
  float espacamentoPlantasM = 0.3f;
  float espacamentoLinhasM = 0.3f;
  float vazaoGotejadorLph = 2.0f;
  int numGotejadoresPlanta = 1;
  int numeroTotalPlantas = 30;
  float limiarUmidadeMinima = 50.0f; // umidade do solo (%) abaixo da qual o modo automatico rega
};

struct StateSnapshot {
  String modo;
  bool autorizado;
  float quantidadeAgua;
  float tempMin;
  float tempMax;
  bool emExecucao;
  float latitude;
};

// Estado compartilhado entre as tasks; todo acesso passa pelo mutex _mutex.
class IrrigationState {
public:
  IrrigationState()
      : _modo("manual"), _autorizado(false), _quantidadeAgua(0.0f),
        _tempMin(INFINITY), _tempMax(-INFINITY), _emExecucao(false),
        _latitude(0.0f) {
    _mutex = xSemaphoreCreateMutex();
  }

  String getModo() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    String valor = _modo;
    xSemaphoreGive(_mutex);
    return valor;
  }

  void setModo(const String &modo) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _modo = modo;
    xSemaphoreGive(_mutex);
  }

  bool getAutorizado() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool valor = _autorizado;
    xSemaphoreGive(_mutex);
    return valor;
  }

  void setAutorizado(bool autorizado) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _autorizado = autorizado;
    xSemaphoreGive(_mutex);
  }

  float getQuantidadeAgua() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    float valor = _quantidadeAgua;
    xSemaphoreGive(_mutex);
    return valor;
  }

  void setQuantidadeAgua(float quantidade) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _quantidadeAgua = quantidade;
    xSemaphoreGive(_mutex);
  }

  float getLatitude() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    float valor = _latitude;
    xSemaphoreGive(_mutex);
    return valor;
  }

  void setLatitude(float latitude) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _latitude = latitude;
    xSemaphoreGive(_mutex);
  }

  bool getEmExecucao() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    bool valor = _emExecucao;
    xSemaphoreGive(_mutex);
    return valor;
  }

  void setEmExecucao(bool emExecucao) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    _emExecucao = emExecucao;
    xSemaphoreGive(_mutex);
  }

  void registrarTemperatura(float temperatura) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (temperatura < _tempMin) _tempMin = temperatura;
    if (temperatura > _tempMax) _tempMax = temperatura;
    xSemaphoreGive(_mutex);
  }

  float getTempMin() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    float valor = _tempMin;
    xSemaphoreGive(_mutex);
    return valor;
  }

  float getTempMax() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    float valor = _tempMax;
    xSemaphoreGive(_mutex);
    return valor;
  }

  ConfigIrrigacao getConfigIrrigacao() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    ConfigIrrigacao valor = _configIrrigacao;
    xSemaphoreGive(_mutex);
    return valor;
  }

  // Campos em snake_case porque espelham os nomes usados pela API.
  void atualizarConfigIrrigacao(JsonObjectConst novosParametros) {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    if (!novosParametros["kc"].isNull())
      _configIrrigacao.kc = novosParametros["kc"].as<float>();
    if (!novosParametros["espacamento_plantas_m"].isNull())
      _configIrrigacao.espacamentoPlantasM = novosParametros["espacamento_plantas_m"].as<float>();
    if (!novosParametros["espacamento_linhas_m"].isNull())
      _configIrrigacao.espacamentoLinhasM = novosParametros["espacamento_linhas_m"].as<float>();
    if (!novosParametros["vazao_gotejador_lph"].isNull())
      _configIrrigacao.vazaoGotejadorLph = novosParametros["vazao_gotejador_lph"].as<float>();
    if (!novosParametros["num_gotejadores_planta"].isNull())
      _configIrrigacao.numGotejadoresPlanta = novosParametros["num_gotejadores_planta"].as<int>();
    if (!novosParametros["numero_total_plantas"].isNull())
      _configIrrigacao.numeroTotalPlantas = novosParametros["numero_total_plantas"].as<int>();
    if (!novosParametros["limiar_umidade_minima"].isNull())
      _configIrrigacao.limiarUmidadeMinima = novosParametros["limiar_umidade_minima"].as<float>();
    xSemaphoreGive(_mutex);
  }

  StateSnapshot snapshot() {
    xSemaphoreTake(_mutex, portMAX_DELAY);
    StateSnapshot valor;
    valor.modo = _modo;
    valor.autorizado = _autorizado;
    valor.quantidadeAgua = _quantidadeAgua;
    valor.tempMin = _tempMin;
    valor.tempMax = _tempMax;
    valor.emExecucao = _emExecucao;
    valor.latitude = _latitude;
    xSemaphoreGive(_mutex);
    return valor;
  }

private:
  SemaphoreHandle_t _mutex;
  String _modo;
  bool _autorizado;
  float _quantidadeAgua;
  float _tempMin;
  float _tempMax;
  bool _emExecucao;
  float _latitude;
  ConfigIrrigacao _configIrrigacao;
};

static IrrigationState state;

// ==================== Rele ====================
namespace Relay {
  bool _ligado = false;

  void begin() {
    pinMode(RELAY_GPIO_PIN, OUTPUT);
    digitalWrite(RELAY_GPIO_PIN, LOW); // comeca desligado por seguranca
    _ligado = false;
  }

  void ligar() {
    digitalWrite(RELAY_GPIO_PIN, HIGH);
    _ligado = true;
    Serial.println("Rele ligado");
  }

  void desligar() {
    digitalWrite(RELAY_GPIO_PIN, LOW);
    _ligado = false;
    Serial.println("Rele desligado");
  }

  bool estaLigado() {
    return _ligado;
  }
}

// ==================== Sensor DHT (temperatura/umidade do ar) ====================
namespace DhtSensor {
  DHT _dht(DHT_PIN, DHT_SENSOR_TYPE);

  void begin() {
    _dht.begin();
  }

  bool readTemperature(float &outValue) {
    for (int tentativa = 1; tentativa <= DHT_READ_RETRIES; tentativa++) {
      float valor = _dht.readTemperature();
      if (!isnan(valor)) {
        outValue = valor;
        return true;
      }
      Serial.printf("Falha ao ler temperatura do DHT11 (%d/%d)\n", tentativa, DHT_READ_RETRIES);
      vTaskDelay(pdMS_TO_TICKS(DHT_READ_RETRY_DELAY_SECONDS * 1000));
    }
    Serial.println("Leitura de temperatura falhou apos todas as tentativas.");
    return false;
  }

  bool readHumidity(float &outValue) {
    for (int tentativa = 1; tentativa <= DHT_READ_RETRIES; tentativa++) {
      float valor = _dht.readHumidity();
      if (!isnan(valor)) {
        outValue = valor;
        return true;
      }
      Serial.printf("Falha ao ler umidade do DHT11 (%d/%d)\n", tentativa, DHT_READ_RETRIES);
      vTaskDelay(pdMS_TO_TICKS(DHT_READ_RETRY_DELAY_SECONDS * 1000));
    }
    Serial.println("Leitura de umidade falhou apos todas as tentativas.");
    return false;
  }
}

// ==================== Sensor de umidade do solo ====================
namespace SoilSensor {
  esp_adc_cal_characteristics_t _adcCalibracao;
  const int AMOSTRAS = 16;

  float lerTensaoMedia() {
    uint32_t somaMv = 0;
    for (int i = 0; i < AMOSTRAS; i++) {
      uint32_t leituraBruta = analogRead(SOIL_ADC_PIN);
      somaMv += esp_adc_cal_raw_to_voltage(leituraBruta, &_adcCalibracao);
    }
    return (somaMv / (float)AMOSTRAS) / 1000.0f; // mV -> V
  }

  void begin() {
    analogSetAttenuation(ADC_11db);
    analogReadResolution(12);
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &_adcCalibracao);
  }

  bool readSoilPercent(float &outValue) {
    float tensao = lerTensaoMedia();
    float umidade = ((SOIL_MAX_VOLTAGE - tensao) / (SOIL_MAX_VOLTAGE - SOIL_MIN_VOLTAGE)) * 100.0f;
    outValue = constrain(umidade, 0.0f, 100.0f);
    return true;
  }
}

// ==================== Display LCD ====================
namespace Display {
  LiquidCrystal_I2C _lcd(LCD_I2C_ADDRESS, LCD_COLS, LCD_ROWS);

  void begin() {
    if (!LCD_ENABLED) return;
    _lcd.init();
    _lcd.backlight();
    _lcd.clear();
  }

  void exibir(float temperature, float humidity, float soilPercent, const String &modo, float limiar) {
    if (!LCD_ENABLED) return;
    (void)soilPercent;
    (void)limiar;

    char linha1[17];
    char linha2[17];
    String modoAbrev = modo.substring(0, 3);

    snprintf(linha1, sizeof(linha1), "Temp=%.1fC %s", temperature, modoAbrev.c_str());
    snprintf(linha2, sizeof(linha2), "Umid=%.1f%%", humidity);

    _lcd.clear();
    _lcd.setCursor(0, 0);
    _lcd.print(linha1);
    _lcd.setCursor(0, 1);
    _lcd.print(linha2);
  }
}

// ==================== Calculo de ETo / irrigacao ====================

struct IrrigationResult {
  float tempoSegundos;
  float volumePorPlanta;
};

// Formula de Hargreaves-Samani para ETo diaria (mm/dia).
float calcularEto(float tempMin, float tempMax, float latitude, int diaDoAno) {
  float tempMed = (tempMin + tempMax) / 2.0f;
  float latRad = latitude * (float)M_PI / 180.0f;

  float dr = 1.0f + 0.033f * cosf(2.0f * (float)M_PI / 365.0f * diaDoAno);
  float delta = 0.409f * sinf((2.0f * (float)M_PI / 365.0f * diaDoAno) - 1.39f);

  float cosOmegaS = -tanf(latRad) * tanf(delta);
  cosOmegaS = std::max(-1.0f, std::min(1.0f, cosOmegaS)); // evita erro numerico em latitudes extremas
  float omegaS = acosf(cosOmegaS);

  const float Gsc = 0.0820f;
  float raMj = (24.0f * 60.0f / (float)M_PI) * Gsc * dr *
               (omegaS * sinf(latRad) * sinf(delta) + cosf(latRad) * cosf(delta) * sinf(omegaS));
  float raMm = raMj * 0.408f;

  const float C = 0.0023f;
  float td = std::max(0.0f, tempMax - tempMin);

  return C * raMm * sqrtf(td) * (tempMed + 17.8f);
}

// Tempo (s) e volume (L) por planta, a partir da ETo e dos parametros da cultura.
IrrigationResult calcularIrrigacao(
    float etoMm,
    float kc,
    float espacamentoPlantasM,
    float espacamentoLinhasM,
    float vazaoGotejadorLph,
    int numGotejadoresPlanta
) {
  float etcMmDia = etoMm * kc;
  float areaPorPlantaM2 = espacamentoPlantasM * espacamentoLinhasM;
  float volumeLitrosPorPlanta = etcMmDia * areaPorPlantaM2; // 1mm sobre 1m2 = 1 litro
  float vazaoTotalPorPlantaLph = vazaoGotejadorLph * numGotejadoresPlanta;

  if (vazaoTotalPorPlantaLph == 0.0f) {
    return {0.0f, 0.0f};
  }

  float tempoIrrigacaoHoras = volumeLitrosPorPlanta / vazaoTotalPorPlantaLph;
  float tempoIrrigacaoSegundos = tempoIrrigacaoHoras * 3600.0f;

  return {tempoIrrigacaoSegundos, volumeLitrosPorPlanta};
}

// ==================== Cliente HTTP da API ====================

struct IrrigationHistoryRecord {
  float umidadeAtual;
  String tempo;
  float volumePorPlanta;
  float volumeTotal;
  String data;
  String hora;
  String modo;
};

namespace ApiClient {
  // Backoff exponencial entre tentativas: 2s, 4s, 8s... ate HTTP_MAX_RETRIES.
  bool postWithRetry(const String &url, const JsonDocument &payload) {
    String body;
    serializeJson(payload, body);

    for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
      HTTPClient http;
      http.begin(url);
      http.addHeader("Content-Type", "application/json");
      http.addHeader("X-Device-Key", DEVICE_API_KEY);
      http.setTimeout(API_TIMEOUT_SECONDS * 1000);

      int statusCode = http.POST(body);
      http.end();

      if (statusCode >= 200 && statusCode < 300) {
        return true;
      }

      float wait = HTTP_BACKOFF_BASE_SECONDS * (1 << min(attempt - 1, 5));
      Serial.printf("Falha ao enviar para %s (tentativa %d/%d): HTTP %d. Nova tentativa em %.1fs.\n",
                    url.c_str(), attempt, HTTP_MAX_RETRIES, statusCode, wait);
      vTaskDelay(pdMS_TO_TICKS((int)(wait * 1000)));
    }

    Serial.printf("Todas as tentativas de envio para %s falharam. Payload descartado.\n", url.c_str());
    return false;
  }

  bool sendTelemetry(float temperature, float humidity, float soilPercent) {
    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["soilPercent"] = soilPercent;
    doc["timestamp"] = (double)time(nullptr);

    String url = String(API_BASE_URL) + "/api/webhooks/telemetry";
    return postWithRetry(url, doc);
  }

  bool sendStatus(const StateSnapshot &status) {
    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["modo"] = status.modo;
    doc["autorizado"] = status.autorizado;
    doc["quantidadeAgua"] = status.quantidadeAgua;
    doc["tempMin"] = status.tempMin;
    doc["tempMax"] = status.tempMax;
    doc["emExecucao"] = status.emExecucao;
    doc["latitude"] = status.latitude;

    String url = String(API_BASE_URL) + "/api/webhooks/status";
    return postWithRetry(url, doc);
  }

  bool sendIrrigationHistory(const IrrigationHistoryRecord &record) {
    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["umidadeAtual"] = record.umidadeAtual;
    doc["tempo"] = record.tempo;
    doc["volumePorPlanta"] = record.volumePorPlanta;
    doc["volumeTotal"] = record.volumeTotal;
    doc["data"] = record.data;
    doc["hora"] = record.hora;
    doc["modo"] = record.modo;

    String url = String(API_BASE_URL) + "/api/webhooks/history";
    return postWithRetry(url, doc);
  }

  bool fetchInitialConfig(JsonDocument &outConfig) {
    String url = String(API_BASE_URL) + "/api/devices/" + DEVICE_ID + "/config";

    HTTPClient http;
    http.begin(url);
    http.addHeader("X-Device-Key", DEVICE_API_KEY);
    http.setTimeout(API_TIMEOUT_SECONDS * 1000);

    int statusCode = http.GET();
    if (statusCode != 200) {
      Serial.printf("Erro ao buscar configuracao inicial da API: HTTP %d\n", statusCode);
      http.end();
      return false;
    }

    DeserializationError err = deserializeJson(outConfig, http.getString());
    http.end();

    if (err) {
      Serial.printf("Erro ao parsear configuracao inicial: %s\n", err.c_str());
      return false;
    }

    return true;
  }
}

// ==================== Long-polling de comandos ====================

using CommandHandler = std::function<void(JsonObjectConst payload)>;

namespace CommandPoller {
  std::map<String, CommandHandler> _handlers;
  volatile bool _running = false;

  void dispatch(const String &jsonPayload) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, jsonPayload);
    if (err) {
      Serial.printf("Erro ao parsear comando: %s\n", err.c_str());
      return;
    }

    String type = doc["type"].isNull() ? String("") : doc["type"].as<String>();
    auto it = _handlers.find(type);
    if (it == _handlers.end()) {
      Serial.printf("Comando desconhecido recebido: %s\n", type.c_str());
      return;
    }

    JsonObjectConst payload = doc["payload"].is<JsonObject>()
        ? doc["payload"].as<JsonObjectConst>()
        : JsonObjectConst();
    it->second(payload);
  }

  void pollLoop(void *pvParameters) {
    int consecutiveErrors = 0;

    while (_running) {
      HTTPClient http;
      String url = String(API_BASE_URL) + "/api/devices/" + DEVICE_ID +
                    "/commands/poll?deviceId=" + DEVICE_ID;
      http.begin(url);
      http.addHeader("X-Device-Key", DEVICE_API_KEY);
      http.setTimeout((LONG_POLL_TIMEOUT_SECONDS + 5) * 1000);

      int statusCode = http.GET();

      if (statusCode == 204) {
        consecutiveErrors = 0;
      } else if (statusCode == 200) {
        consecutiveErrors = 0;
        dispatch(http.getString());
      } else if (statusCode == HTTPC_ERROR_READ_TIMEOUT) {
        consecutiveErrors = 0; // timeout normal do long-poll
      } else {
        consecutiveErrors++;
        int backoff = min(30, 1 << min(consecutiveErrors, 5));
        Serial.printf("Erro no long polling de comandos: HTTP %d. Nova tentativa em %ds.\n",
                      statusCode, backoff);
        http.end();
        vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
        continue;
      }

      http.end();
    }

    vTaskDelete(nullptr);
  }

  void registerHandler(const String &type, CommandHandler handler) {
    _handlers[type] = handler;
  }

  void begin() {
    _running = true;
    xTaskCreate(pollLoop, "commandPollTask", 8192, nullptr, 1, nullptr);
    Serial.println("Long polling de comandos iniciado.");
  }
}

namespace IrrigationRunner {
  bool start(float tempoSegundos, float volumePorPlanta, float volumeTotal, float umidadeAntes, const String &modo);
  void requestStop();
}

// ==================== Servico de irrigacao (manual/automatico) ====================
namespace IrrigationService {
  int diaDoAno() {
    time_t agora = time(nullptr);
    struct tm horario;
    localtime_r(&agora, &horario);
    return horario.tm_yday + 1; // tm_yday e 0-indexado
  }

  String dataFormatada() {
    time_t agora = time(nullptr);
    struct tm horario;
    localtime_r(&agora, &horario);
    char buf[11];
    strftime(buf, sizeof(buf), "%d/%m/%Y", &horario);
    return String(buf);
  }

  String horaFormatada() {
    time_t agora = time(nullptr);
    struct tm horario;
    localtime_r(&agora, &horario);
    char buf[9];
    strftime(buf, sizeof(buf), "%H:%M:%S", &horario);
    return String(buf);
  }

  String tempoFormatado(long totalSegundos) {
    int horas = totalSegundos / 3600;
    int minutos = (totalSegundos % 3600) / 60;
    int segundos = totalSegundos % 60;
    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", horas, minutos, segundos);
    return String(buf);
  }

  void autorizarIrrigacao(bool autorizado) {
    state.setAutorizado(autorizado);
    ApiClient::sendStatus(state.snapshot());
    Serial.printf("Irrigacao automatica %s\n", autorizado ? "autorizada" : "nao autorizada");
  }

  void definirModo(const String &modo) {
    String modoLower = modo;
    modoLower.toLowerCase();
    state.setModo(modoLower);
    ApiClient::sendStatus(state.snapshot());
    Serial.printf("Modo de irrigacao definido para %s\n", modoLower.c_str());
  }

  void definirLatitude(float latitude) {
    state.setLatitude(latitude);
    ApiClient::sendStatus(state.snapshot());
  }

  void atualizarConfigIrrigacao(JsonObjectConst novosParametros) {
    state.atualizarConfigIrrigacao(novosParametros);
    Serial.println("Parametros de irrigacao atualizados.");
  }

  void executarModoAutomatico() {
    ConfigIrrigacao config = state.getConfigIrrigacao();

    float umidadeAtual;
    if (!SoilSensor::readSoilPercent(umidadeAtual)) {
      return;
    }

    float limiar = config.limiarUmidadeMinima;
    float eto = calcularEto(state.getTempMin(), state.getTempMax(), state.getLatitude(), diaDoAno());

    Serial.printf("Modo automatico: umidade=%.1f%%, limiar=%.1f%%\n", umidadeAtual, limiar);

    if (umidadeAtual >= limiar) {
      Serial.println("Umidade do solo adequada. Nenhuma acao tomada.");
      return;
    }

    IrrigationResult resultado = calcularIrrigacao(
        eto,
        config.kc,
        config.espacamentoPlantasM,
        config.espacamentoLinhasM,
        config.vazaoGotejadorLph,
        config.numGotejadoresPlanta
    );
    float volumeTotal = resultado.volumePorPlanta * config.numeroTotalPlantas;

    if (resultado.tempoSegundos <= 0) {
      Serial.println("Solo abaixo do limiar, mas o calculo de irrigacao deu 0 (vazao do gotejador configurada como 0?). Nenhuma acao.");
      return;
    }

    state.setQuantidadeAgua(volumeTotal);
    Serial.printf("Disparando irrigacao automatica: %.1fs, %.2fL/planta, %.2fL total.\n",
                  resultado.tempoSegundos, resultado.volumePorPlanta, volumeTotal);
    IrrigationRunner::start(resultado.tempoSegundos, resultado.volumePorPlanta, volumeTotal, umidadeAtual, "automatico");
  }

  void executarModoManual(float quantidadeAguaLitros) {
    ConfigIrrigacao config = state.getConfigIrrigacao();
    state.setQuantidadeAgua(quantidadeAguaLitros);

    float umidadeAntes;
    if (!SoilSensor::readSoilPercent(umidadeAntes)) {
      umidadeAntes = 0.0f;
    }

    float vazaoTotalLph = config.vazaoGotejadorLph * config.numGotejadoresPlanta * config.numeroTotalPlantas;
    if (vazaoTotalLph <= 0.0f) {
      Serial.println("Nao foi possivel calcular a duracao da irrigacao manual (vazao configurada como 0?).");
      return;
    }
    float tempoSegundos = (quantidadeAguaLitros / vazaoTotalLph) * 3600.0f;
    float volumePorPlanta = config.numeroTotalPlantas > 0
        ? quantidadeAguaLitros / config.numeroTotalPlantas
        : quantidadeAguaLitros;

    Serial.printf("Disparando irrigacao manual: %.2fL, duracao estimada %.1fs.\n", quantidadeAguaLitros, tempoSegundos);
    IrrigationRunner::start(tempoSegundos, volumePorPlanta, quantidadeAguaLitros, umidadeAntes, "manual");
  }
}

// ==================== Execucao da irrigacao (task dedicada) ====================
namespace IrrigationRunner {
  volatile bool _stopRequested = false;

  // So existe uma irrigacao por vez, garantido por start() checar
  // state.getEmExecucao() antes de criar a task.
  struct Job {
    float tempoSegundos;
    float volumePorPlanta;
    float volumeTotal;
    float umidadeAntes;
    String modo;
  };
  static Job _job;

  void task(void *pvParameters) {
    Relay::ligar();
    ApiClient::sendStatus(state.snapshot());

    const long passoMs = 500; // granularidade da checagem de interrupcao
    long totalMs = (long)(_job.tempoSegundos * 1000);
    long decorridoMs = 0;
    while (decorridoMs < totalMs && !_stopRequested) {
      long espera = std::min((long)passoMs, totalMs - decorridoMs);
      vTaskDelay(pdMS_TO_TICKS(espera));
      decorridoMs += espera;
    }

    Relay::desligar();
    state.setEmExecucao(false);
    ApiClient::sendStatus(state.snapshot());

    IrrigationHistoryRecord record;
    record.umidadeAtual = roundf(_job.umidadeAntes * 10) / 10.0f;
    record.tempo = IrrigationService::tempoFormatado(decorridoMs / 1000);
    record.volumePorPlanta = roundf(_job.volumePorPlanta * 100) / 100.0f;
    record.volumeTotal = roundf(_job.volumeTotal * 100) / 100.0f;
    record.data = IrrigationService::dataFormatada();
    record.hora = IrrigationService::horaFormatada();
    record.modo = _job.modo;
    ApiClient::sendIrrigationHistory(record);

    Serial.printf("Irrigacao (%s) concluida. Duracao real: %lds.\n", _job.modo.c_str(), decorridoMs / 1000);
    vTaskDelete(nullptr);
  }

  bool start(float tempoSegundos, float volumePorPlanta, float volumeTotal, float umidadeAntes, const String &modo) {
    if (state.getEmExecucao()) {
      Serial.println("Irrigacao ignorada: ja ha uma irrigacao em andamento.");
      return false;
    }
    _job = {tempoSegundos, volumePorPlanta, volumeTotal, umidadeAntes, modo};
    _stopRequested = false;
    state.setEmExecucao(true);
    xTaskCreate(task, "irrigationTask", 8192, nullptr, 1, nullptr);
    return true;
  }

  void requestStop() {
    _stopRequested = true;
  }
}

// ==================== Handlers de comando ====================
void handleStartIrrigation(JsonObjectConst payload) {
  if (payload["quantidade_agua"].isNull()) {
    Serial.println("START_IRRIGATION sem quantidade_agua no payload, ignorando.");
    return;
  }
  float quantidadeAgua = payload["quantidade_agua"].as<float>();

  if (state.getModo() != "manual") {
    Serial.println("START_IRRIGATION ignorado: dispositivo nao esta em modo manual.");
    return;
  }
  if (state.getEmExecucao()) {
    Serial.println("START_IRRIGATION ignorado: ja ha uma irrigacao em andamento.");
    return;
  }
  IrrigationService::executarModoManual(quantidadeAgua);
}

void handleStopIrrigation(JsonObjectConst payload) {
  (void)payload;
  if (state.getEmExecucao()) {
    IrrigationRunner::requestStop();
    Serial.println("STOP_IRRIGATION recebido, solicitando parada da irrigacao em andamento.");
  } else {
    Serial.println("STOP_IRRIGATION recebido, mas nao ha irrigacao em andamento.");
  }
}

void handleDefinirModo(JsonObjectConst payload) {
  String modo = payload["modo"].isNull() ? state.getModo() : payload["modo"].as<String>();
  IrrigationService::definirModo(modo);
}

void handleAutorizarIrrigacao(JsonObjectConst payload) {
  bool autorizado = payload["autorizado"].isNull() ? false : payload["autorizado"].as<bool>();
  IrrigationService::autorizarIrrigacao(autorizado);
}

void handleConfigIrrigacao(JsonObjectConst payload) {
  IrrigationService::atualizarConfigIrrigacao(payload);
}

void handleLatitude(JsonObjectConst payload) {
  float latitude = payload["latitude"].isNull() ? state.getLatitude() : payload["latitude"].as<float>();
  IrrigationService::definirLatitude(latitude);
}

// ==================== Tasks FreeRTOS ====================
void sensorTask(void *pvParameters) {
  for (;;) {
    float temperature, humidity, soilPercent;
    bool okTemp = DhtSensor::readTemperature(temperature);
    bool okHum = DhtSensor::readHumidity(humidity);
    bool okSoil = SoilSensor::readSoilPercent(soilPercent);

    if (!okTemp || !okHum || !okSoil) {
      vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_SECONDS * 1000));
      continue;
    }

    state.registrarTemperatura(temperature);
    float limiar = state.getConfigIrrigacao().limiarUmidadeMinima;

    Serial.printf("Temp=%.1fC Umid.Ar=%.1f%% Umid.Solo=%.1f%% Modo=%s\n",
                  temperature, humidity, soilPercent, state.getModo().c_str());

    ApiClient::sendTelemetry(temperature, humidity, soilPercent);
    Display::exibir(temperature, humidity, soilPercent, state.getModo(), limiar);

    if (!state.getEmExecucao() && state.getModo() == "automatico" &&
        state.getAutorizado() && soilPercent < limiar) {
      IrrigationService::executarModoAutomatico();
    }

    vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_SECONDS * 1000));
  }
}

void statusSyncTask(void *pvParameters) {
  for (;;) {
    ApiClient::sendStatus(state.snapshot());
    vTaskDelay(pdMS_TO_TICKS(STATUS_SYNC_INTERVAL_SECONDS * 1000));
  }
}

// ==================== Execucao do programa ====================
void conectarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando ao WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi conectado, IP: ");
  Serial.println(WiFi.localIP());
  WiFi.setAutoReconnect(true);
}

void sincronizarRelogio() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sincronizando hora via NTP");

  const unsigned long NTP_TIMEOUT_MS = 30000;
  unsigned long inicio = millis();
  time_t agora = time(nullptr);

  while (agora < 8 * 3600 * 2) {
    if (millis() - inicio > NTP_TIMEOUT_MS) {
      Serial.println();
      Serial.println("Timeout aguardando sincronizacao NTP. Prosseguindo com hora nao sincronizada.");
      return;
    }
    delay(500);
    Serial.print(".");
    agora = time(nullptr);
  }
  Serial.println();
}

void carregarConfiguracaoInicial() {
  JsonDocument doc;
  if (!ApiClient::fetchInitialConfig(doc)) {
    Serial.println("Configuracao inicial indisponivel na API. Usando valores padrao locais.");
    return;
  }

  if (!doc["modo"].isNull()) state.setModo(doc["modo"].as<String>());
  if (!doc["autorizado"].isNull()) state.setAutorizado(doc["autorizado"].as<bool>());
  if (!doc["quantidadeAgua"].isNull()) state.setQuantidadeAgua(doc["quantidadeAgua"].as<float>());
  if (!doc["latitude"].isNull()) state.setLatitude(doc["latitude"].as<float>());
  if (doc["configIrrigacao"].is<JsonObject>()) {
    state.atualizarConfigIrrigacao(doc["configIrrigacao"].as<JsonObject>());
  }

  Serial.println("Configuracao inicial carregada da API.");
}

void setup() {
  Serial.begin(115200);
  delay(1000); // da tempo do monitor serial conectar antes dos primeiros logs

  conectarWifi();
  sincronizarRelogio();
  setenv("TZ", TZ_STRING, 1); // configTime() acima sempre traz UTC; aplica o fuso local depois
  tzset();

  Relay::begin();
  DhtSensor::begin();
  SoilSensor::begin();
  Display::begin();

  carregarConfiguracaoInicial();

  CommandPoller::registerHandler("START_IRRIGATION", handleStartIrrigation);
  CommandPoller::registerHandler("STOP_IRRIGATION", handleStopIrrigation);
  CommandPoller::registerHandler("MODE_CHANGE", handleDefinirModo);
  CommandPoller::registerHandler("AUTHORIZATION_CHANGE", handleAutorizarIrrigacao);
  CommandPoller::registerHandler("CONFIG_IRRIGACAO", handleConfigIrrigacao);
  CommandPoller::registerHandler("LATITUDE", handleLatitude);
  CommandPoller::begin();

  xTaskCreate(sensorTask, "sensorTask", 8192, nullptr, 1, nullptr);
  xTaskCreate(statusSyncTask, "statusSyncTask", 4096, nullptr, 1, nullptr);

  Serial.println("Controlador de irrigacao inicializado.");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
