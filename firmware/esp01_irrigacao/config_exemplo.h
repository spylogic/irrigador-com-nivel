/*
  Copie este arquivo para "config.h" (mesma pasta) e preencha com os
  seus dados. O arquivo "config.h" NÃO deve ser enviado ao GitHub
  (ele já está no .gitignore do projeto) porque tem a senha do WiFi.
*/
#pragma once

// ---------------- WiFi ----------------
#define WIFI_SSID     "NOME_DA_SUA_REDE"
#define WIFI_SENHA    "SENHA_DA_SUA_REDE"

// ---------------- Firebase Realtime Database ----------------
// Pegue essa URL no Console do Firebase > Realtime Database.
// Formato geral: https://SEU-PROJETO-default-rtdb.SUA-REGIAO.firebasedatabase.app
// (sem barra "/" no final)
#define FIREBASE_DATABASE_URL "https://SEU-PROJETO-default-rtdb.firebaseio.com"

// Se você configurar regras do banco exigindo uma chave de acesso
// (recomendado depois que tudo estiver funcionando), coloque aqui e
// ela será enviada como "?auth=" nas chamadas. Deixe "" para banco
// aberto (modo de teste).
#define FIREBASE_AUTH_TOKEN ""
