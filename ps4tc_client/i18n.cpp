#include "i18n.h"
#include "app.h"
#include "httpc.h"
#include "json.h"
#include "log.h"

#include <map>

// Язык системы PS4: sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &id).
extern "C" int sceSystemServiceParamGetInt(int paramId, int* value);
static const int PARAM_ID_LANG = 1;
static const int LANG_ID_RUSSIAN = 8;

static std::string g_want = "en";     // какой файл просим: "ru" или "en"
static bool        g_loaded = false;  // true: либо загрузили, либо решили остаться на английском
static uint64_t    g_lastTryUs = 0;
static std::map<std::string, std::string> g_strings;

void i18n_init()
{
    int id = -1;
    int r = sceSystemServiceParamGetInt(PARAM_ID_LANG, &id);
    if (r == 0 && id == LANG_ID_RUSSIAN) g_want = "ru";
    else g_want = "en";
    logf_("i18n: system language id %d (rc 0x%08X) -> %s", id, (unsigned)r, g_want.c_str());
}

void i18n_tick(bool serverOk, uint64_t nowUs)
{
    if (g_loaded || !serverOk) return;
    if (g_lastTryUs != 0 && nowUs - g_lastTryUs < 5000000ULL) return;
    g_lastTryUs = nowUs;

    HttpResult r = http_get(g_host, g_port, "/lang/" + g_want + ".json", 3000);
    if (!r.connected) return;   // демон пока недоступен: попробуем позже

    JsonValue j;
    if (r.status == 200 && json_parse(r.body, j) && j.type == JsonValue::OBJ) {
        g_strings.clear();
        for (size_t i = 0; i < j.obj.size(); i++)
            if (j.obj[i].second.type == JsonValue::STR) g_strings[j.obj[i].first] = j.obj[i].second.str;
        logf_("i18n: loaded /lang/%s.json (%d strings)", g_want.c_str(), (int)g_strings.size());
    } else {
        logf_("i18n: /lang/%s.json unavailable (status %d), using built-in English", g_want.c_str(), r.status);
    }
    g_loaded = true;   // при любой ошибке остаётся встроенный английский
}

std::string tr(const char* key, const char* def)
{
    std::map<std::string, std::string>::const_iterator it = g_strings.find(key);
    if (it != g_strings.end() && !it->second.empty()) return it->second;
    return def;
}
