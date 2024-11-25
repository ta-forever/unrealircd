#include "unrealircd.h"
#include <curl/curl.h>
#include <jansson.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define PERSPECTIVE_API_URL "https://commentanalyzer.googleapis.com/v1alpha1/comments:analyze?key="
#define TAG_NAME "taforever.com/toxicity"

ModuleHeader MOD_HEADER
= {
  "third/perspective_api",
  "1.0",
  "Google Perspective API toxicity filter",
  "TA Forever",
  "unrealircd-6",
};

typedef struct {
    Client* client;
    Channel* channel;   // exactly one of channel or target is NULL
    Client* target;     // depending on whether chanmsg or usermsg
    MessageTag* mtags;
    char* text;
    double toxicity_score;
    bool completed;
    char completion_error[2048];
} WorkingMessage;

// Linked list node to store completed messages
typedef struct QueueNode {
    struct QueueNode* next;
    WorkingMessage* message;
} QueueNode;

// Queue structure
typedef struct {
    QueueNode* head;
    QueueNode* tail;
} Queue;

static Queue* message_queue = NULL;

// HOOKTYPE_NEW_MESSAGE
static void new_message_hook(Client* sender, MessageTag* recv_mtags, MessageTag** mtag_list, const char* signature);

// HOOKTYPE_PRE_USERMSG
static int pre_user_message_hook(Client* client, Client* target, MessageTag** mtags, const char* text, SendType sendtype);

// HOOKTYPE_PRE_CHANMSG
static int pre_channel_message_hook(Client* client, Channel* channel, MessageTag** mtags, const char* text, SendType sendtype);

// HOOKTYPE_PROCESS_CLIENTS
static int process_clients_hook();

//config
static int perspective_api_config_test(ConfigFile* cf, ConfigEntry *ce, int type, int* errs);
static int perspective_api_config_run(ConfigFile* cf, ConfigEntry* ce, int type);

// implementation
static int pre_channel_or_user_message_hook(Client* client, Channel* channel, Client* target, MessageTag** mtags, const char* text, SendType sendtype);
static void* toxicity_worker(void* arg);
static double get_toxicity_score(const char* text, char* completion_error, int completion_error_buffer_size);
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp);
static char* get_perspective_api_key();
static int is_module_disabled();

// Queue operations
static Queue* queue_create();
static int queue_is_empty(Queue* queue);
static WorkingMessage* queue_dequeue(Queue* queue);

// Variables to track error state and re-enable time
static int consecutive_errors = 0;
static time_t module_disabled_until = 0;
static int error_threshold = 3;
static int disable_duration = 3600;

MOD_TEST()
{
    HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, perspective_api_config_test);
    return MOD_SUCCESS;
}

MOD_INIT()
{
    curl_global_init(CURL_GLOBAL_ALL);
    MARK_AS_GLOBAL_MODULE(modinfo);

    MessageTagHandlerInfo mtag;
    memset(&mtag, 0, sizeof(mtag));
    mtag.name = TAG_NAME;
    mtag.flags = MTAG_HANDLER_FLAGS_NO_CAP_NEEDED;
    MessageTagHandlerAdd(modinfo->handle, &mtag);

    message_queue = queue_create();

    HookAddVoid(modinfo->handle, HOOKTYPE_NEW_MESSAGE, 0, new_message_hook);
    HookAdd(modinfo->handle, HOOKTYPE_PRE_CHANMSG, 0, pre_channel_message_hook);
    HookAdd(modinfo->handle, HOOKTYPE_PRE_USERMSG, 0, pre_user_message_hook);
    HookAdd(modinfo->handle, HOOKTYPE_PROCESS_CLIENTS, 0, process_clients_hook);
    HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN, 0, perspective_api_config_run);

    return MOD_SUCCESS;
}

MOD_LOAD()
{
    return MOD_SUCCESS;
}

MOD_UNLOAD()
{
    while (!queue_is_empty(message_queue)) {
        WorkingMessage* msg_data = queue_dequeue(message_queue);
        free_message_tags(msg_data->mtags);
        free(msg_data->text);
        free(msg_data);
    }
    free(message_queue);
    return MOD_SUCCESS;
}

int perspective_api_config_test(ConfigFile* cf, ConfigEntry *ce, int type, int* errs)
{
    int errors = 0;
    ConfigEntry* cep;

    if (type != CONFIG_SET)
        return 0;

    if (!ce || !ce->name || strcmp(ce->name, "perspective_api"))
        return 0;

    for (cep = ce->items; cep; cep = cep->next)
    {
        if (!cep->value)
        {
            config_error("%s:%i: set::perspective_api::%s with no value",
                cep->file->filename, cep->line_number, cep->name);
            errors++;
        }
        else if (!strcmp(cep->name, "error-threshold"))
        {
            int v = atoi(cep->value);
            if (v == 0)
            {
                config_error("%s:%i: set::perspective_api::error-threshold: must be a non-zero integer (-ve to disable. got: %d)",
                    cep->file->filename, cep->line_number, v);
                errors++;
            }
        }
        else if (!strcmp(cep->name, "disable-duration"))
        {
        }
        else
        {
            config_error("%s:%i: unknown directive set::perspective_api::%s",
                cep->file->filename, cep->line_number, cep->name);
            errors++;
        }
    }
    *errs = errors;
    return errors ? -1 : 1;
}

int perspective_api_config_run(ConfigFile* cf, ConfigEntry* ce, int type)
{
    ConfigEntry* cep;

    if (type != CONFIG_SET)
        return 0;

    if (!ce || !ce->name || strcmp(ce->name, "perspective_api"))
        return 0;

    for (cep = ce->items; cep; cep = cep->next)
    {
        if (!strcmp(cep->name, "error-threshold"))
        {
            error_threshold = atoi(cep->value);
        }
        else if (!strcmp(cep->name, "disable-duration"))
        {
            disable_duration = config_checkval(cep->value, CFG_TIME);
        }
    }
    return 1;
}

void new_message_hook(Client* sender, MessageTag* recv_mtags, MessageTag** mtag_list, const char* signature)
{
    unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[new_message_hook]");
    MessageTag* m = find_mtag(recv_mtags, TAG_NAME);
    if (m)
    {
        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[new_message_hook] copying toxicity tag ...");
        MessageTag* new_m = safe_alloc(sizeof(MessageTag));
        new_m->name = our_strdup(TAG_NAME);
        new_m->value = our_strdup(m->value);
        AddListItem(new_m, *mtag_list);
    }
    unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[new_message_hook] DONE");
}

int pre_user_message_hook(Client* client, Client* target, MessageTag** mtags, const char* text, SendType sendtype)
{
    return pre_channel_or_user_message_hook(client, NULL, target, mtags, text, sendtype);
}

int pre_channel_message_hook(Client* client, Channel* channel, MessageTag** mtags, const char* text, SendType sendtype)
{
    return pre_channel_or_user_message_hook(client, channel, NULL, mtags, text, sendtype);
}

int pre_channel_or_user_message_hook(Client* client, Channel* channel, Client* target, MessageTag** mtags, const char* text, SendType sendtype)
{
    if (is_module_disabled()) {
        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] returning early because perspective API is disabled");
        return HOOK_CONTINUE;
    }

    if (!text || !IsUser(client) || !mtags || !*mtags)
    {
        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] returning early because !text || !IsUser(client) || !mtags || !*mtags");
        return HOOK_CONTINUE;
    }

    {
        MessageTag* m;
        for (m = *mtags; m != NULL; m = m->next)
        {
            unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] initial mtags: $name=$value",
                log_data_string("name", m->name),
                log_data_string("value", m->value));
        }
    }

    MessageTag* m = find_mtag(*mtags, TAG_NAME);
    if (m)
    {
        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] returning early because toxicity tag already present");
        return HOOK_CONTINUE;
    }

    // Create a new WorkingMessage struct to hold the message data
    unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] creating toxicity workload");
    WorkingMessage* msg_data = (WorkingMessage*)malloc(sizeof(WorkingMessage));
    msg_data->client = client;
    msg_data->channel = channel;
    msg_data->target = target;
    msg_data->mtags = *mtags;
    msg_data->text = our_strdup(text);
    msg_data->toxicity_score = -1.0;
    msg_data->completed = false;
    memset(msg_data->completion_error, 0, sizeof(msg_data->completion_error));

    // append message to working queue
    unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] appending workload to queue");
    QueueNode* new_node = malloc(sizeof(QueueNode));
    new_node->message = msg_data;
    new_node->next = NULL;
    if (message_queue->tail) {
        message_queue->tail->next = new_node;
    }
    else {
        message_queue->head = new_node;
    }
    message_queue->tail = new_node;

    // Create a worker thread to process the toxicity check
    unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] creating worker thread");
    pthread_t worker_thread;
    pthread_create(&worker_thread, NULL, toxicity_worker, (void*)msg_data);
    pthread_detach(worker_thread);

    unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[pre_channel_or_user_message_hook] DONE");
    return HOOK_DEFER;
}

void* toxicity_worker(void* arg)
{
    WorkingMessage* msg_data = (WorkingMessage*)arg;
    msg_data->toxicity_score = get_toxicity_score(msg_data->text, msg_data->completion_error, sizeof(msg_data->completion_error)-1);
    msg_data->completed = true;
    return NULL;
}

int process_clients_hook()
{
    while (!queue_is_empty(message_queue) && message_queue->head->message && message_queue->head->message->completed) {
        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[process_clients_hook] popping complete message from message_queue");
        WorkingMessage* msg_data = queue_dequeue(message_queue);

        if (msg_data->completion_error[0] != '\0') {
            consecutive_errors++;
            unreal_log(ULOG_ERROR, "perspective_api", "ERROR_TRACKING", NULL,
                "Error retrieving toxicity: $message (Error count: $count)",
                log_data_string("message", msg_data->completion_error),
                log_data_integer("count", consecutive_errors));

            if (consecutive_errors >= error_threshold) {
                module_disabled_until = time(NULL) + disable_duration;
                unreal_log(ULOG_ERROR, "perspective_api", "MODULE_DISABLED", NULL,
                    "Disabling Perspective API for $duration seconds due to $threshold consecutive errors.",
                    log_data_integer("duration", disable_duration),
                    log_data_integer("threshold", error_threshold));
            }
        }
        else {
            unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[process_clients_hook] complete message has no errors. resetting consective_errors");
            consecutive_errors = 0; // Reset error counter on success
        }

        // Toxicity score is attached regardless of validity.  we don't try again
        char toxicity_score_str[16] = { '\0' };
        snprintf(toxicity_score_str, sizeof(toxicity_score_str), "%.2f", msg_data->toxicity_score);

        MessageTag* m = find_mtag(msg_data->mtags, TAG_NAME);
        if (m) {
            unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[process_clients_hook] overwriting existing toxicity score: $toxicity",
                log_data_string("toxicity", toxicity_score_str));
            safe_strdup(m->value, toxicity_score_str);
        }
        else {
            unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[process_clients_hook] attached new toxicity score: $toxicity",
                log_data_string("toxicity", toxicity_score_str));
            m = safe_alloc(sizeof(MessageTag));
            m->name = our_strdup(TAG_NAME);
            m->value = our_strdup(toxicity_score_str);
            AddListItem(m, msg_data->mtags);
        }

        // Prepare parameters for the PRIVMSG command
        const char* parv[3];
        int parc = 3;

        parv[0] = msg_data->client->name;   // Source (sender)
        parv[2] = msg_data->text;          // Message text
        if (msg_data->channel) {
            parv[1] = msg_data->channel->name; // Target (channel)
        }
        else if (msg_data->target) {
            parv[1] = msg_data->target->name; // Target (channel)
        }
        else {
            unreal_log(ULOG_ERROR, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "Exactly one of channel or target must be non-NULL!");
        }

        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[process_clients_hook] re-dispatching PRIVMSG");
        do_cmd(msg_data->client, msg_data->mtags, "PRIVMSG", parc, parv);

        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[process_clients_hook] freeing resources");
        free_message_tags(msg_data->mtags);
        free(msg_data->text);
        free(msg_data);
        unreal_log(ULOG_DEBUG, "perspective_api", "PERSPECTIVE_API_DEBUG", NULL, "[process_clients_hook] done with this completed message");
    }

    return 0;
}

double get_toxicity_score(const char* text, char* completion_error, int completion_error_buffer_size)
{
    CURLcode res;
    struct curl_slist* headers = NULL;
    char postdata[1024];
    double toxicity_score = -1;
    char response_data[2048] = "";
    completion_error[0] = '\0';

    char* api_key = get_perspective_api_key();
    if (!api_key)
    {
        strncpy(completion_error, "Unable to get PERSPECTIVE_API_KEY env variable", completion_error_buffer_size);
        return toxicity_score;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        strncpy(completion_error, "Unable to curl_easy_init()!", completion_error_buffer_size);
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

    response_data[sizeof(response_data) - 1] = '\0';
    if (strstr(response_data, "LANGUAGE_NOT_SUPPORTED_BY_ATTRIBUTE") != NULL) {
        curl_easy_cleanup(curl);
        return toxicity_score;
    }

    if (res == CURLE_OK) {
        json_t* root;
        json_error_t error;

        root = json_loads(response_data, 0, &error);
        if (root) {
            json_t* attribute_scores = json_object_get(root, "attributeScores");
            if (attribute_scores) {
                json_t* toxicity_obj = json_object_get(attribute_scores, "TOXICITY");
                if (toxicity_obj) {
                    json_t* summary_score = json_object_get(toxicity_obj, "summaryScore");
                    if (summary_score) {
                        json_t* value_obj = json_object_get(summary_score, "value");
                        if (value_obj && json_is_number(value_obj)) {
                            toxicity_score = json_number_value(value_obj);
                        }
                        else {
                            snprintf(completion_error, completion_error_buffer_size, "attributeScores.TOXICITY.summaryScore.value not found or is not a number: %s", response_data);
                        }
                    }
                    else {
                        snprintf(completion_error, completion_error_buffer_size, "attributeScores.TOXICITY.summaryScore not found: %s", response_data);
                    }
                }
                else {
                    snprintf(completion_error, completion_error_buffer_size, "attributeScores.TOXICITY not found: %s", response_data);
                }
            }
            else {
                snprintf(completion_error, completion_error_buffer_size, "attributeScores not found: %s", response_data);
            }
            json_decref(root);
        }
        else {
            snprintf(completion_error, completion_error_buffer_size, "Failed to parse: %s", response_data);
        }
    }
    else {
        snprintf(completion_error, completion_error_buffer_size, "CURL error: %s", curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    return toxicity_score;
}

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp)
{
    strcat((char*)userp, (char*)contents);
    return size * nmemb;
}

static char* get_perspective_api_key()
{
    char* api_key = getenv("PERSPECTIVE_API_KEY");
    if (!api_key) {
        return NULL;
    }
    return api_key;
}

Queue* queue_create()
{
    Queue* queue = malloc(sizeof(Queue));
    queue->head = NULL;
    queue->tail = NULL;
    return queue;
}

int queue_is_empty(Queue* queue)
{
    return queue->head == NULL;
}

WorkingMessage* queue_dequeue(Queue* queue)
{
    if (queue_is_empty(queue)) {
        return NULL;
    }
    QueueNode* node = queue->head;
    WorkingMessage* msg_data = node->message;
    queue->head = node->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    free(node);
    return msg_data;
}

int is_module_disabled()
{
    return time(NULL) < module_disabled_until;
}
