// ====================================================================
// Configuração do Firebase para o DASHBOARD (site)
// ====================================================================
// Pegue esses valores no Console do Firebase:
//   Configurações do projeto > Geral > Seus apps > SDK setup and configuration
//
// Esses valores (inclusive o apiKey) NÃO são segredos - a segurança do
// banco é feita pelas "Regras" do Realtime Database, não por esconder
// essa configuração. Por isso é normal (e comum) commitar este arquivo
// no GitHub junto com o resto do site.
// ====================================================================

const firebaseConfig = {
  apiKey: "SUA_API_KEY",
  authDomain: "SEU-PROJETO.firebaseapp.com",
  databaseURL: "https://SEU-PROJETO-default-rtdb.firebaseio.com",
  projectId: "SEU-PROJETO",
  storageBucket: "SEU-PROJETO.appspot.com",
  messagingSenderId: "SEU_SENDER_ID",
  appId: "SEU_APP_ID"
};
