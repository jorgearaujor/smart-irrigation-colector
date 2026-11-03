// Modelo de configuracao do coletor.
// Copie este arquivo para "secrets.h" (mesma pasta do .ino) e preencha.
// O secrets.h esta no .gitignore: nunca faca commit dele.

#pragma once

// Rede Wi-Fi do local onde o coletor fica instalado
#define WIFI_SSID ""
#define WIFI_PASSWORD ""

// Endereco da API, sem "/api" no final. Ex.: "https://minha-api.onrender.com"
#define API_BASE_URL ""

// Identificador do dispositivo (o painel web usa "esp32-01")
#define DEVICE_ID "esp32-01"

// Precisa ser IGUAL ao DEVICE_API_KEY configurado na API.
// Para gerar uma chave nova: openssl rand -hex 32
#define DEVICE_API_KEY ""
