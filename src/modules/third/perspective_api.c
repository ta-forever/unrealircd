#include "unrealircd.h"
#include <curl/curl.h>
#include <jansson.h>

#define PERSPECTIVE_API_URL "https://commentanalyzer.googleapis.com/v1alpha1/comments:analyze?key="

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp);
static double get_toxicity_score(const char *text);
static char *get_perspective_api_key();

ModuleHeader MOD_HEADER
  = {
	"third/perspective_api",
	"1.0",
	"Google Perspective API toxicity filter",
	"TA Forever",
	"unrealircd-6",
	};

int channel_message_hook(Client *client, Channel *channel, MessageTag **mtags, const char *text, SendType sendtype)
{
	if (!text || !IsUser(client) || !mtags || !*mtags)
	{
		return 0;
	}

	double toxicity_score = get_toxicity_score(text);
	if (toxicity_score < 0.0)
	{
		return 0;
	}

	char toxicity_score_str[16] = {'\0'};
	snprintf(toxicity_score_str, sizeof(toxicity_score_str), "%.2f", toxicity_score);

	MessageTag *m = find_mtag(*mtags, "taforever.com/toxicity");
	if (m)
	{
		safe_strdup(m->value, toxicity_score_str);
	}
	else
	{
		m = safe_alloc(sizeof(MessageTag));
		safe_strdup(m->name, "taforever.com/toxicity");
		safe_strdup(m->value, toxicity_score_str);
		AddListItem(m, *mtags);
	}
	return 0;
}

MOD_INIT()
{
	curl_global_init(CURL_GLOBAL_ALL);
	MARK_AS_GLOBAL_MODULE(modinfo);

	MessageTagHandlerInfo mtag;
	memset(&mtag, 0, sizeof(mtag));
	mtag.name = "taforever.com/toxicity";
	mtag.flags = MTAG_HANDLER_FLAGS_NO_CAP_NEEDED;
	MessageTagHandlerAdd(modinfo->handle, &mtag);

    HookAdd(modinfo->handle, HOOKTYPE_PRE_CHANMSG, 0, channel_message_hook);
    return MOD_SUCCESS;
}

MOD_LOAD()
{
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	return MOD_SUCCESS;
}

double get_toxicity_score(const char *text) {

	CURLcode res;
	struct curl_slist *headers = NULL;
	char postdata[1024];
	double toxicity_score = -1;
	char response_data[2048] = "";

	char *api_key = get_perspective_api_key();

	CURL *curl = curl_easy_init();
	if (!curl) {
		unreal_log(ULOG_ERROR, "third/perspective_api", "PERSPECTIVE_API_CURL_EASY_INIT_FAIL", NULL, "Unable to curl_easy_init()!");
		return toxicity_score;
	}

	snprintf(postdata, sizeof(postdata),
         "{\"comment\": {\"text\": \"%s\"}, \"requestedAttributes\": {\"TOXICITY\": {}}}",
         text);
	headers = curl_slist_append(headers, "Content-Type: application/json");

	// Build the full URL with the API key
	char api_url[1024];
	snprintf(api_url, sizeof(api_url), "%s%s", PERSPECTIVE_API_URL, api_key);

	curl_easy_setopt(curl, CURLOPT_URL, api_url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_data);

	res = curl_easy_perform(curl);

	if (res == CURLE_OK) {
		json_t *root;
		json_error_t error;

		root = json_loads(response_data, 0, &error);
		unreal_log(ULOG_DEBUG, "third/perspective_api", "PERSPECTIVE_API_RESPONSE", NULL, response_data);
		if (root) {
			json_t *attribute_scores = json_object_get(root, "attributeScores");
			json_t *toxicity_obj = json_object_get(attribute_scores, "TOXICITY");
			json_t *summary_score = json_object_get(toxicity_obj, "summaryScore");
			toxicity_score = json_number_value(json_object_get(summary_score, "value"));
			json_decref(root);
		}
		else
		{
			unreal_log(ULOG_ERROR, "third/perspective_api", "PERSPECTIVE_API_JSON_PARSE_FAIL", NULL, "unable to parse Perspective API response");
		}
	} else {
		unreal_log(ULOG_ERROR, "third/perspective_api", "PERSPECTIVE_API_REQUEST_FAIL", NULL, "request failed: $err",
			log_data_string("err", curl_easy_strerror(res)));
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return toxicity_score;
}

static char *get_perspective_api_key() {
	char *api_key = getenv("PERSPECTIVE_API_KEY");
	if (!api_key) {
		fprintf(stderr, "Error: No Perspective API key found\n");
		return NULL;
	}

	return api_key;
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
	strncat(userp, contents, size * nmemb);
	return size * nmemb;
}
