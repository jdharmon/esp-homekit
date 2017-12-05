#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <esp/hwrand.h>
#include <espressif/esp_system.h>

#include <lwip/sockets.h>

#include <unistd.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

#include "mdnsresponder.h"
#include <http-parser/http_parser.h>
#include <cJSON.h>

#include "crypto.h"
#include "tlv.h"
#include "pairing.h"
#include "storage.h"
#include "query_params.h"
#include "debug.h"

#include "homekit/types.h"
#include "homekit/characteristics.h"


#define PORT 5556


struct _client_context_t;
typedef struct _client_context_t client_context_t;


typedef enum {
    HOMEKIT_ENDPOINT_UNKNOWN = 0,
    HOMEKIT_ENDPOINT_PAIR_SETUP,
    HOMEKIT_ENDPOINT_PAIR_VERIFY,
    HOMEKIT_ENDPOINT_IDENTIFY,
    HOMEKIT_ENDPOINT_GET_ACCESSORIES,
    HOMEKIT_ENDPOINT_GET_CHARACTERISTICS,
    HOMEKIT_ENDPOINT_UPDATE_CHARACTERISTICS,
    HOMEKIT_ENDPOINT_PAIRINGS,
    HOMEKIT_ENDPOINT_RESET,
} homekit_endpoint_t;


typedef struct {
    Srp *srp;
    byte *public_key;
    size_t public_key_size;

    client_context_t *client;
} pairing_context_t;


typedef struct {
    byte *secret;
    size_t secret_size;
    byte *session_key;
    size_t session_key_size;
    byte *device_public_key;
    size_t device_public_key_size;
    byte *accessory_public_key;
    size_t accessory_public_key_size;
} pair_verify_context_t;


typedef struct {
    char *accessory_id;
    ed25519_key *accessory_key;

    homekit_accessory_t **accessories;

    bool paired;
    pairing_context_t *pairing_context;

    client_context_t *clients;
} server_t;


struct _client_context_t {
    server_t *server;
    int socket;
    homekit_endpoint_t endpoint;
    query_param_t *endpoint_params;

    char *body;
    size_t body_length;

    int pairing_id;
    byte permissions;

    bool disconnect;

    bool encrypted;
    byte *read_key;
    byte *write_key;
    int count_reads;
    int count_writes;

    QueueHandle_t event_queue;
    pair_verify_context_t *verify_context;

    struct _client_context_t *next;
};


void client_context_free(client_context_t *c);
void pairing_context_free(pairing_context_t *context);


server_t *server_new() {
    server_t *server = malloc(sizeof(server_t));
    server->accessory_id = NULL;
    server->accessory_key = NULL;
    server->accessories = NULL;
    server->paired = false;
    server->pairing_context = NULL;
    server->clients = NULL;
    return server;
}


void server_free(server_t *server) {
    if (server->accessory_id)
        free(server->accessory_id);

    if (server->accessory_key)
        crypto_ed25519_free(server->accessory_key);

    if (server->pairing_context)
        pairing_context_free(server->pairing_context);

    if (server->clients) {
        client_context_t *client = server->clients;
        while (client) {
            client_context_t *next = client->next;
            client_context_free(client);
            client = next;
        }
    }

    free(server);
}


void tlv_debug(const tlv_values_t *values) {
    DEBUG("Got following TLV values:");
    for (tlv_t *t=values->head; t; t=t->next) {
        char *escaped_payload = binary_to_string(t->value, t->size);
        DEBUG("Type %d value (%d bytes): %s", t->type, t->size, escaped_payload);
        free(escaped_payload);
    }
}


void mdns_txt_add(char* txt, size_t txt_size, const char* key, const char* value)
{
    size_t txt_len = strlen(txt),
           key_len = strlen(key),
           value_len = strlen(value);

    size_t extra_len = key_len + value_len + 1;  // extra 1 is for equals sign

    if (extra_len > 255) {
        printf(">>> mdns_txt_add: key %s section is longer than 255\n", key);
        return;
    }

    if (txt_len + extra_len + 2 > txt_size) {  // extra 2 is for length and terminator
        printf(">>> mdns_txt_add: not enough space to add TXT key %s\n", key);
        return;
    }

    txt[txt_len] = extra_len;
    snprintf(txt + txt_len + 1, txt_size - txt_len, "%s=%s", key, value);
}


typedef enum {
    TLVType_Method = 0,        // (integer) Method to use for pairing. See PairMethod
    TLVType_Identifier = 1,    // (UTF-8) Identifier for authentication
    TLVType_Salt = 2,          // (bytes) 16+ bytes of random salt
    TLVType_PublicKey = 3,     // (bytes) Curve25519, SRP public key or signed Ed25519 key
    TLVType_Proof = 4,         // (bytes) Ed25519 or SRP proof
    TLVType_EncryptedData = 5, // (bytes) Encrypted data with auth tag at end
    TLVType_State = 6,         // (integer) State of the pairing process. 1=M1, 2=M2, etc.
    TLVType_Error = 7,         // (integer) Error code. Must only be present if error code is
                               // not 0. See TLVError
    TLVType_RetryDelay = 8,    // (integer) Seconds to delay until retrying a setup code
    TLVType_Certificate = 9,   // (bytes) X.509 Certificate
    TLVType_Signature = 10,    // (bytes) Ed25519
    TLVType_Permissions = 11,  // (integer) Bit value describing permissions of the controller
                               // being added.
                               // None (0x00): Regular user
                               // Bit 1 (0x01): Admin that is able to add and remove
                               // pairings against the accessory
    TLVType_FragmentData = 13, // (bytes) Non-last fragment of data. If length is 0,
                               // it's an ACK.
    TLVType_FragmentLast = 14, // (bytes) Last fragment of data
    TLVType_Separator = 0xff,
} TLVType;


typedef enum {
  TLVMethod_PairSetup = 1,
  TLVMethod_PairVerify = 2,
  TLVMethod_AddPairing = 3,
  TLVMethod_RemovePairing = 4,
  TLVMethod_ListPairings = 5,
} TLVMethod;


typedef enum {
  TLVError_Unknown = 1,         // Generic error to handle unexpected errors
  TLVError_Authentication = 2,  // Setup code or signature verification failed
  TLVError_Backoff = 3,         // Client must look at the retry delay TLV item and
                                // wait that many seconds before retrying
  TLVError_MaxPeers = 4,        // Server cannot accept any more pairings
  TLVError_MaxTries = 5,        // Server reached its maximum number of
                                // authentication attempts
  TLVError_Unavailable = 6,     // Server pairing method is unavailable
  TLVError_Busy = 7,            // Server is busy and cannot accept a pairing
                                // request at this time
} TLVError;


typedef enum {
    // This specifies a success for the request
    HAPStatus_Success = 0,
    // Request denied due to insufficient privileges
    HAPStatus_InsufficientPrivileges = -70401,
    // Unable to communicate with requested services,
    // e.g. the power to the accessory was turned off
    HAPStatus_NoAccessoryConnection = -70402,
    // Resource is busy, try again
    HAPStatus_ResourceBusy = -70403,
    // Connot write to read only characteristic
    HAPStatus_ReadOnly = -70404,
    // Cannot read from a write only characteristic
    HAPStatus_WriteOnly = -70405,
    // Notification is not supported for characteristic
    HAPStatus_NotificationsUnsupported = -70406,
    // Out of resources to process request
    HAPStatus_OutOfResources = -70407,
    // Operation timed out
    HAPStatus_Timeout = -70408,
    // Resource does not exist
    HAPStatus_NoResource = -70409,
    // Accessory received an invalid value in a write request
    HAPStatus_InvalidValue = -70410,
    // Insufficient Authorization
    HAPStatus_InsufficientAuthorization = -70411,
} HAPStatus;


pair_verify_context_t *pair_verify_context_new() {
    pair_verify_context_t *context = malloc(sizeof(pair_verify_context_t));

    context->secret = NULL;
    context->secret_size = 0;

    context->session_key = NULL;
    context->session_key_size = 0;
    context->device_public_key = NULL;
    context->device_public_key_size = 0;
    context->accessory_public_key = NULL;
    context->accessory_public_key_size = 0;

    return context;
}

void pair_verify_context_free(pair_verify_context_t *context) {
    if (context->secret)
        free(context->secret);

    if (context->session_key)
        free(context->session_key);

    if (context->device_public_key)
        free(context->device_public_key);

    if (context->accessory_public_key)
        free(context->accessory_public_key);

    free(context);
}


client_context_t *client_context_new() {
    client_context_t *c = malloc(sizeof(client_context_t));
    c->server = NULL;
    c->endpoint_params = NULL;
    c->body = NULL;
    c->body_length = 0;

    c->pairing_id = -1;
    c->encrypted = false;
    c->read_key = NULL;
    c->write_key = NULL;
    c->count_reads = 0;
    c->count_writes = 0;

    c->disconnect = false;

    c->event_queue = xQueueCreate(20, sizeof(homekit_characteristic_t*));
    c->verify_context = NULL;

    c->next = NULL;

    return c;
}


void client_context_free(client_context_t *c) {
    if (c->read_key)
        free(c->read_key);

    if (c->write_key)
        free(c->write_key);

    if (c->verify_context)
        pair_verify_context_free(c->verify_context);

    if (c->event_queue)
        vQueueDelete(c->event_queue);

    if (c->endpoint_params)
        query_params_free(c->endpoint_params);

    if (c->body)
        free(c->body);

    free(c);
}




pairing_context_t *pairing_context_new() {
    pairing_context_t *context = malloc(sizeof(pairing_context_t));
    context->srp = crypto_srp_new();
    context->client = NULL;
    context->public_key = NULL;
    context->public_key_size = 0;
    return context;
}

void pairing_context_free(pairing_context_t *context) {
    if (context->srp) {
        crypto_srp_free(context->srp);
    }
    if (context->public_key) {
        free(context->public_key);
    }
    free(context);
}


void client_notify_characteristic(homekit_characteristic_t *ch, void *client);


typedef enum {
    characteristic_format_type   = (1 << 1),
    characteristic_format_meta   = (1 << 2),
    characteristic_format_perms  = (1 << 3),
    characteristic_format_events = (1 << 4),
} characteristic_format_t;


typedef bool (*bool_getter)();
typedef int (*int_getter)();
typedef float (*float_getter)();
typedef const char *(*string_getter)();

typedef void (*bool_setter)(bool);
typedef void (*int_setter)(int);
typedef void (*float_setter)(float);
typedef void (*string_setter)(const char *);


cJSON *characteristic_to_json(client_context_t *client, homekit_characteristic_t *ch, characteristic_format_t format) {
    cJSON *j_ch = cJSON_CreateObject();
    cJSON_AddNumberToObject(j_ch, "aid", ch->service->accessory->id);
    cJSON_AddNumberToObject(j_ch, "iid", ch->id);

    if (format & characteristic_format_type) {
        cJSON_AddStringToObject(j_ch, "type", ch->type);
    }

    if (format & characteristic_format_perms) {
        cJSON *j_perms = cJSON_CreateArray();
        cJSON_AddItemToObject(j_ch, "perms", j_perms);
        if (ch->permissions & homekit_permissions_paired_read)
            cJSON_AddItemToArray(j_perms, cJSON_CreateString("pr"));
        if (ch->permissions & homekit_permissions_paired_write)
            cJSON_AddItemToArray(j_perms, cJSON_CreateString("pw"));
        if (ch->permissions & homekit_permissions_notify)
            cJSON_AddItemToArray(j_perms, cJSON_CreateString("ev"));
        if (ch->permissions & homekit_permissions_additional_authorization)
            cJSON_AddItemToArray(j_perms, cJSON_CreateString("aa"));
        if (ch->permissions & homekit_permissions_timed_write)
            cJSON_AddItemToArray(j_perms, cJSON_CreateString("tw"));
        if (ch->permissions & homekit_permissions_hidden)
            cJSON_AddItemToArray(j_perms, cJSON_CreateString("hd"));
    }

    if ((format & characteristic_format_events) && (ch->permissions & homekit_permissions_notify)) {
        bool events = homekit_characteristic_has_notify_callback(ch, client_notify_characteristic, client);
        cJSON_AddItemToObject(j_ch, "ev", cJSON_CreateBool(events));
    }

    if (format & characteristic_format_meta) {
        if (ch->description)
            cJSON_AddStringToObject(j_ch, "description", ch->description);

        const char *format_str = NULL;
        switch(ch->format) {
            case homekit_format_bool: format_str = "bool"; break;
            case homekit_format_uint8: format_str = "uint8"; break;
            case homekit_format_uint16: format_str = "uint16"; break;
            case homekit_format_uint32: format_str = "uint32"; break;
            case homekit_format_uint64: format_str = "uint64"; break;
            case homekit_format_int: format_str = "int"; break;
            case homekit_format_float: format_str = "float"; break;
            case homekit_format_string: format_str = "string"; break;
            case homekit_format_tlv: format_str = "tlv"; break;
            case homekit_format_data: format_str = "data"; break;
        }
        if (format_str)
            cJSON_AddStringToObject(j_ch, "format", format_str);

        const char *unit_str = NULL;
        switch(ch->unit) {
            case homekit_unit_none: break;
            case homekit_unit_celsius: unit_str = "celsius"; break;
            case homekit_unit_percentage: unit_str = "percentage"; break;
            case homekit_unit_arcdegrees: unit_str = "arcdegrees"; break;
            case homekit_unit_lux: unit_str = "lux"; break;
            case homekit_unit_seconds: unit_str = "seconds"; break;
        }
        if (unit_str)
            cJSON_AddStringToObject(j_ch, "unit", unit_str);

        if (ch->min_value)
            cJSON_AddNumberToObject(j_ch, "minValue", *ch->min_value);

        if (ch->max_value)
            cJSON_AddNumberToObject(j_ch, "maxValue", *ch->max_value);

        if (ch->min_step)
            cJSON_AddNumberToObject(j_ch, "minStep", *ch->min_step);

        if (ch->max_len)
            cJSON_AddNumberToObject(j_ch, "maxLen", *ch->max_len);

        if (ch->max_data_len)
            cJSON_AddNumberToObject(j_ch, "maxDataLen", *ch->max_data_len);
    }

    cJSON *j_value = NULL;
    if (ch->permissions & homekit_permissions_paired_read) {
        switch(ch->format) {
            case homekit_format_bool: {
                // DEBUG("Getting value for bool \"%s\", getter = %p", ch->type, ch->getter);
                j_value = cJSON_CreateBool(
                    (ch->getter) ? ((bool_getter)ch->getter)() : ch->bool_value
                );
                break;
            }
            case homekit_format_uint8:
            case homekit_format_uint16:
            case homekit_format_uint32:
            case homekit_format_uint64:
            case homekit_format_int: {
                // DEBUG("Getting value for int \"%s\", getter = %p", ch->type, ch->getter);
                j_value = cJSON_CreateNumber(
                    (ch->getter) ? ((int_getter)ch->getter)() : ch->int_value
                );
                break;
            }
            case homekit_format_float: {
                // DEBUG("Getting value for float \"%s\", getter = %p", ch->type, ch->getter);
                j_value = cJSON_CreateNumber(
                    (ch->getter) ? ((float_getter)ch->getter)() : ch->float_value
                );
                break;
            }
            case homekit_format_string: {
                // DEBUG("Getting value for string \"%s\", getter = %p", ch->type, ch->getter);
                j_value = cJSON_CreateString(
                    (ch->getter) ? ((string_getter)ch->getter)() : ch->string_value
                );
                break;
            }
            case homekit_format_tlv:
            case homekit_format_data:
                // TODO:
                break;
        }
    }
    if (j_value)
        cJSON_AddItemToObject(j_ch, "value", j_value);

    return j_ch;
}


int client_encrypt(
    client_context_t *context,
    byte *payload, size_t size,
    byte *encrypted, size_t *encrypted_size
) {
    if (!context || !context->encrypted || !context->read_key)
        return -1;

    size_t required_encrypted_size = size + (size + 1023) / 1024 * 18;
    if (*encrypted_size < required_encrypted_size) {
        *encrypted_size = required_encrypted_size;
        return -2;
    }

    *encrypted_size = required_encrypted_size;

    byte nonce[12];
    memset(nonce, 0, sizeof(nonce));

    int payload_offset = 0;
    int encrypted_offset = 0;

    while (payload_offset < size) {
        size_t chunk_size = size - payload_offset;
        if (chunk_size > 1024)
            chunk_size = 1024;

        byte aead[2] = {chunk_size % 256, chunk_size / 256};

        memcpy(encrypted+encrypted_offset, aead, 2);

        byte i = 4;
        int x = context->count_reads++;
        while (x) {
            nonce[i++] = x % 256;
            x /= 256;
        }

        size_t available = *encrypted_size - encrypted_offset - 2;
        int r = crypto_chacha20poly1305_encrypt(
            context->read_key, nonce, aead, 2,
            payload+payload_offset, chunk_size,
            encrypted+encrypted_offset+2, &available
        );
        if (r) {
            DEBUG("Failed to chacha encrypt payload (code %d)", r);
            return -1;
        }

        payload_offset += chunk_size;
        encrypted_offset += available + 2;
    }

    return 0;
}


int client_decrypt(
    client_context_t *context,
    byte *payload, size_t payload_size,
    byte *decrypted, size_t *decrypted_size
) {
    if (!context || !context->encrypted || !context->write_key)
        return -1;

    const size_t block_size = 1024 + 16 + 2;
    size_t required_decrypted_size =
        payload_size / block_size * 1024 + payload_size % block_size - 16 - 2;
    if (*decrypted_size < required_decrypted_size) {
        *decrypted_size = required_decrypted_size;
        return -2;
    }

    *decrypted_size = required_decrypted_size;

    byte nonce[12];
    memset(nonce, 0, sizeof(nonce));

    int payload_offset = 0;
    int decrypted_offset = 0;

    while (payload_offset < payload_size) {
        size_t chunk_size = payload[payload_offset] + payload[payload_offset+1]*256;
        if (chunk_size+18 > payload_size-payload_offset) {
            // Unfinished chunk
            break;
        }

        byte i = 4;
        int x = context->count_writes++;
        while (x) {
            nonce[i++] = x % 256;
            x /= 256;
        }

        size_t decrypted_len = *decrypted_size - decrypted_offset;
        int r = crypto_chacha20poly1305_decrypt(
            context->write_key, nonce, payload+payload_offset, 2,
            payload+payload_offset+2, chunk_size + 16,
            decrypted, &decrypted_len
        );
        if (r) {
            DEBUG("Failed to chacha decrypt payload (code %d)", r);
            return -1;
        }

        decrypted_offset += decrypted_len;
        payload_offset += chunk_size + 0x12; // TODO: 0x10 is for some auth bytes
    }

    return payload_offset;
}


void client_notify_characteristic(homekit_characteristic_t *ch, void *context) {
    client_context_t *client = context;
    if (client->event_queue) {
        xQueueSendToBack(client->event_queue, &ch, 10);
    }
}


void client_send(client_context_t *context, byte *data, size_t data_size) {
    byte *payload = data;
    size_t payload_size = data_size;

    if (context->encrypted) {
        DEBUG("Encrypting payload");
        payload_size = 0;
        client_encrypt(context, data, data_size, NULL, &payload_size);

        payload = malloc(payload_size);
        int r = client_encrypt(context, data, data_size, payload, &payload_size);
        if (r) {
            DEBUG("Failed to encrypt response (code %d)", r);
            free(payload);
            return;
        }
    }

    lwip_write(context->socket, payload, payload_size);

    if (context->encrypted) {
        free(payload);
    }
}

void send_204_response(client_context_t *context) {
    static char response[] = "HTTP/1.1 204 No Content\r\n\r\n";
    client_send(context, (byte *)response, sizeof(response)-1);
}


void send_characteristic_event(client_context_t *context, homekit_characteristic_t *ch) {
    DEBUG("Sending EVENT");

    cJSON *json = cJSON_CreateObject();
    cJSON *characteristics = cJSON_CreateArray();
    cJSON_AddItemToObject(json, "characteristics", characteristics);

    cJSON *ch_json = characteristic_to_json(context, ch, 0);
    cJSON_AddItemToArray(characteristics, ch_json);

    char *payload = cJSON_PrintUnformatted(json);
    size_t payload_size = strlen(payload);

    cJSON_Delete(json);

    DEBUG("Payload: %s", payload);

    static char *http_headers =
        "EVENT/1.0 200 OK\r\n"
        "Content-Type: application/hap+json\r\n"
        "Content-Length: %d\r\n\r\n";

    int event_size = strlen(http_headers) + payload_size + 1;
    char *event = malloc(event_size);
    int event_len = snprintf(event, event_size, http_headers, payload_size);

    if (event_size - event_len < payload_size + 1) {
        DEBUG("Incorrect event buffer size %d: headers took %d, payload size %d", event_size, event_len, payload_size);
        free(event);
        free(payload);
        return;
    }
    memcpy(event+event_len, payload, payload_size);
    event_len += payload_size;
    event[event_len] = 0;  // required for debug output

    free(payload);

    DEBUG("Sending EVENT: %s", event);

    client_send(context, (byte *)event, event_len);

    free(event);
}


void send_tlv_response(client_context_t *context, const tlv_values_t *values);

void send_tlv_error_response(client_context_t *context, int state, TLVError error) {
    tlv_values_t *response = tlv_new();
    tlv_add_integer_value(response, TLVType_State, state);
    tlv_add_integer_value(response, TLVType_Error, error);

    send_tlv_response(context, response);

    tlv_free(response);
}


void send_tlv_response(client_context_t *context, const tlv_values_t *values) {
    DEBUG("Sending TLV response");
    tlv_debug(values);

    size_t payload_size = 0;
    tlv_format(values, NULL, &payload_size);

    byte *payload = malloc(payload_size);
    int r = tlv_format(values, payload, &payload_size);
    if (r) {
        DEBUG("Failed to format TLV payload (code %d)", r);
        free(payload);
        return;
    }

    static char *http_headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/pairing+tlv8\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n\r\n";

    int response_size = strlen(http_headers) + payload_size + 32;
    char *response = malloc(response_size);
    int response_len = snprintf(response, response_size, http_headers, payload_size);

    if (response_size - response_len < payload_size + 1) {
        DEBUG("Incorrect response buffer size %d: headers took %d, payload size %d", response_size, response_len, payload_size);
        free(response);
        free(payload);
        return;
    }
    memcpy(response+response_len, payload, payload_size);
    response_len += payload_size;

    free(payload);

    // char *debug_response = binary_to_string((byte *)response, response_len);
    // DEBUG("Sending HTTP response: %s", debug_response);
    // free(debug_response);

    client_send(context, (byte *)response, response_len);

    free(response);
}


void send_json_response(client_context_t *context, int status_code, const cJSON *root) {
    DEBUG("Sending JSON response");

    static char *http_headers =
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/hap+json\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n\r\n";

    char *payload = cJSON_PrintUnformatted(root);
    size_t payload_size = strlen(payload);

    DEBUG("Payload: %s", payload);

    const char *status_text = "OK";
    switch (status_code) {
        case 204: status_text = "No Content"; break;
        case 207: status_text = "Multi-Status"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 422: status_text = "Unprocessable Entity"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 503: status_text = "Service Unavailable"; break;
    }

    int response_size = strlen(http_headers) + payload_size + strlen(status_text) + 32;
    char *response = malloc(response_size);
    int response_len = snprintf(response, response_size, http_headers, status_code, status_text, payload_size);

    if (response_size - response_len < payload_size + 1) {
        DEBUG("Incorrect response buffer size %d: headers took %d, payload size %d", response_size, response_len, payload_size);
        free(response);
        free(payload);
        return;
    }
    memcpy(response+response_len, payload, payload_size);
    response_len += payload_size;
    response[response_len] = 0;  // required for debug output

    free(payload);

    DEBUG("Sending HTTP response: %s", response);

    client_send(context, (byte *)response, response_len);

    free(response);
}


void send_json_error_response(client_context_t *context, int status_code, HAPStatus status) {
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "status", status);

    send_json_response(context, status_code, json);

    cJSON_Delete(json);
}


void homekit_server_on_identify(client_context_t *context) {
    DEBUG("HomeKit Identify");

    if (context->server->paired) {
        // Already paired
        send_json_error_response(context, 400, HAPStatus_InsufficientPrivileges);
        return;
    }

    homekit_characteristic_t *ch_identify =
        homekit_characteristic_find_by_type(context->server->accessories, 1, HOMEKIT_CHARACTERISTIC_IDENTIFY);
    if (!ch_identify) {
        send_json_error_response(context, 400, HAPStatus_InsufficientPrivileges);
        return;
    }

    // TODO: execute identify attribute's callbacks

    send_204_response(context);
}

void homekit_server_on_pair_setup(client_context_t *context, const byte *data, size_t size) {
    DEBUG("HomeKit Pair Setup");
    DEBUG("Free heap: %d", xPortGetFreeHeapSize());

    tlv_values_t *message = tlv_new();
    tlv_parse(data, size, message);

    tlv_debug(message);

    switch(tlv_get_integer_value(message, TLVType_State, -1)) {
        case 1: {
            DEBUG("Pair Setup Step 1/3");
            DEBUG("Free heap: %d", xPortGetFreeHeapSize());
            if (context->server->paired) {
                DEBUG("Refusing to pair: already paired");
                send_tlv_error_response(context, 2, TLVError_Unavailable);
                break;
            }

            if (context->server->pairing_context) {
                if (context->server->pairing_context->client != context) {
                    DEBUG("Refusing to pair: another pairing in progress");
                    send_tlv_error_response(context, 2, TLVError_Busy);
                    break;
                }
            } else {
                context->server->pairing_context = pairing_context_new();
                context->server->pairing_context->client = context;
            }

            DEBUG("Initializing crypto");
            DEBUG("Free heap: %d", xPortGetFreeHeapSize());
            crypto_srp_init(context->server->pairing_context->srp, "Pair-Setup", "111-11-111");

            if (context->server->pairing_context->public_key) {
                free(context->server->pairing_context->public_key);
                context->server->pairing_context->public_key = NULL;
            }
            context->server->pairing_context->public_key_size = 0;
            crypto_srp_get_public_key(context->server->pairing_context->srp, NULL, &context->server->pairing_context->public_key_size);

            context->server->pairing_context->public_key = malloc(context->server->pairing_context->public_key_size);
            int r = crypto_srp_get_public_key(context->server->pairing_context->srp, context->server->pairing_context->public_key, &context->server->pairing_context->public_key_size);
            if (r) {
                DEBUG("Failed to dump SPR public key (code %d)", r);

                pairing_context_free(context->server->pairing_context);
                context->server->pairing_context = NULL;

                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            size_t salt_size = 0;
            crypto_srp_get_salt(context->server->pairing_context->srp, NULL, &salt_size);

            byte *salt = malloc(salt_size);
            r = crypto_srp_get_salt(context->server->pairing_context->srp, salt, &salt_size);
            if (r) {
                DEBUG("Failed to get salt (code %d)", r);

                free(salt);
                pairing_context_free(context->server->pairing_context);
                context->server->pairing_context = NULL;

                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_value(response, TLVType_PublicKey, context->server->pairing_context->public_key, context->server->pairing_context->public_key_size);
            tlv_add_value(response, TLVType_Salt, salt, salt_size);
            tlv_add_integer_value(response, TLVType_State, 2);

            free(salt);

            send_tlv_response(context, response);

            tlv_free(response);
            break;
        }
        case 3: {
            DEBUG("Pair Setup Step 2/3");
            DEBUG("Free heap: %d", xPortGetFreeHeapSize());
            tlv_t *device_public_key = tlv_get_value(message, TLVType_PublicKey);
            if (!device_public_key) {
                DEBUG("Invalid payload: no device public key");
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_t *proof = tlv_get_value(message, TLVType_Proof);
            if (!proof) {
                DEBUG("Invalid payload: no device proof");
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            DEBUG("Computing SRP shared secret");
            DEBUG("Free heap: %d", xPortGetFreeHeapSize());
            int r = crypto_srp_compute_key(
                context->server->pairing_context->srp,
                device_public_key->value, device_public_key->size,
                context->server->pairing_context->public_key,
                context->server->pairing_context->public_key_size
            );
            if (r) {
                DEBUG("Failed to compute SRP shared secret (code %d)", r);
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            free(context->server->pairing_context->public_key);
            context->server->pairing_context->public_key = NULL;
            context->server->pairing_context->public_key_size = 0;

            DEBUG("Verifying peer's proof");
            DEBUG("Free heap: %d", xPortGetFreeHeapSize());
            r = crypto_srp_verify(context->server->pairing_context->srp, proof->value, proof->size);
            if (r) {
                DEBUG("Failed to verify peer's proof (code %d)", r);
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            DEBUG("Generating own proof");
            size_t server_proof_size = 0;
            crypto_srp_get_proof(context->server->pairing_context->srp, NULL, &server_proof_size);

            byte *server_proof = malloc(server_proof_size);
            r = crypto_srp_get_proof(context->server->pairing_context->srp, server_proof, &server_proof_size);

            tlv_values_t *response = tlv_new();
            tlv_add_value(response, TLVType_Proof, server_proof, server_proof_size);
            tlv_add_integer_value(response, TLVType_State, 4);

            free(server_proof);

            send_tlv_response(context, response);

            tlv_free(response);
            break;
        }
        case 5: {
            DEBUG("Pair Setup Step 3/3");
            DEBUG("Free heap: %d", xPortGetFreeHeapSize());

            int r;

            byte shared_secret[HKDF_HASH_SIZE];
            size_t shared_secret_size = sizeof(shared_secret);

            DEBUG("Calculating shared secret");
            const char salt1[] = "Pair-Setup-Encrypt-Salt";
            const char info1[] = "Pair-Setup-Encrypt-Info";
            r = crypto_srp_hkdf(
                context->server->pairing_context->srp,
                (byte *)salt1, sizeof(salt1)-1,
                (byte *)info1, sizeof(info1)-1,
                shared_secret, &shared_secret_size
            );
            if (r) {
                DEBUG("Failed to generate shared secret (code %d)", r);
                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_encrypted_data = tlv_get_value(message, TLVType_EncryptedData);
            if (!tlv_encrypted_data) {
                DEBUG("Invalid payload: no encrypted data");
                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            DEBUG("Decrypting payload");
            size_t decrypted_data_size = 0;
            crypto_chacha20poly1305_decrypt(
                shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg05", NULL, 0,
                tlv_encrypted_data->value, tlv_encrypted_data->size,
                NULL, &decrypted_data_size
            );

            byte *decrypted_data = malloc(decrypted_data_size);
            // TODO: check malloc result
            r = crypto_chacha20poly1305_decrypt(
                shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg05", NULL, 0,
                tlv_encrypted_data->value, tlv_encrypted_data->size,
                decrypted_data, &decrypted_data_size
            );
            if (r) {
                DEBUG("Failed to decrypt data (code %d)", r);

                free(decrypted_data);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_values_t *decrypted_message = tlv_new();
            r = tlv_parse(decrypted_data, decrypted_data_size, decrypted_message);
            if (r) {
                DEBUG("Failed to parse decrypted TLV (code %d)", r);

                tlv_free(decrypted_message);
                free(decrypted_data);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            free(decrypted_data);

            tlv_t *tlv_device_id = tlv_get_value(decrypted_message, TLVType_Identifier);
            if (!tlv_device_id) {
                DEBUG("Invalid encrypted payload: no device identifier");

                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_public_key = tlv_get_value(decrypted_message, TLVType_PublicKey);
            if (!tlv_device_public_key) {
                DEBUG("Invalid encrypted payload: no device public key");

                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_signature = tlv_get_value(decrypted_message, TLVType_Signature);
            if (!tlv_device_signature) {
                DEBUG("Invalid encrypted payload: no device signature");

                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            DEBUG("Importing device public key");
            ed25519_key *device_key = crypto_ed25519_new();
            r = crypto_ed25519_import_public_key(
                device_key,
                tlv_device_public_key->value, tlv_device_public_key->size
            );
            if (r) {
                DEBUG("Failed to import device public Key (code %d)", r);

                crypto_ed25519_free(device_key);
                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            byte device_x[HKDF_HASH_SIZE];
            size_t device_x_size = sizeof(device_x);

            DEBUG("Calculating DeviceX");
            const char salt2[] = "Pair-Setup-Controller-Sign-Salt";
            const char info2[] = "Pair-Setup-Controller-Sign-Info";
            r = crypto_srp_hkdf(
                context->server->pairing_context->srp,
                (byte *)salt2, sizeof(salt2)-1,
                (byte *)info2, sizeof(info2)-1,
                device_x, &device_x_size
            );
            if (r) {
                DEBUG("Failed to generate DeviceX (code %d)", r);

                crypto_ed25519_free(device_key);
                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            size_t device_info_size = device_x_size + tlv_device_id->size + tlv_device_public_key->size;
            byte *device_info = malloc(device_info_size);
            memcpy(device_info,
                   device_x,
                   device_x_size);
            memcpy(device_info + device_x_size,
                   tlv_device_id->value,
                   tlv_device_id->size);
            memcpy(device_info + device_x_size + tlv_device_id->size,
                   tlv_device_public_key->value,
                   tlv_device_public_key->size);

            DEBUG("Verifying device signature");
            r = crypto_ed25519_verify(
                device_key,
                device_info, device_info_size,
                tlv_device_signature->value, tlv_device_signature->size
            );
            if (r) {
                DEBUG("Failed to generate DeviceX (code %d)", r);

                free(device_info);
                crypto_ed25519_free(device_key);
                tlv_free(decrypted_message);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            free(device_info);

            char *device_id = strndup((const char *)tlv_device_id->value, tlv_device_id->size);
            DEBUG("Adding pairing for %s", device_id);
            // TODO: replace with a bit
            homekit_storage_add_pairing(device_id, device_key, 1); // 1 for admin
            free(device_id);

            crypto_ed25519_free(device_key);
            tlv_free(decrypted_message);

            DEBUG("Exporting accessory public key");
            size_t accessory_public_key_size = 0;
            crypto_ed25519_export_public_key(context->server->accessory_key, NULL, &accessory_public_key_size);

            byte *accessory_public_key = malloc(accessory_public_key_size);
            r = crypto_ed25519_export_public_key(context->server->accessory_key, accessory_public_key, &accessory_public_key_size);
            if (r) {
                DEBUG("Failed to export accessory public key (code %d)", r);

                free(accessory_public_key);

                send_tlv_error_response(context, 6, TLVError_Authentication);
                break;
            }

            size_t accessory_id_size = strlen(context->server->accessory_id);
            size_t accessory_info_size = HKDF_HASH_SIZE + accessory_id_size + accessory_public_key_size;
            byte *accessory_info = malloc(accessory_info_size);

            DEBUG("Calculating AccessoryX");
            size_t accessory_x_size = accessory_info_size;
            const char salt3[] = "Pair-Setup-Accessory-Sign-Salt";
            const char info3[] = "Pair-Setup-Accessory-Sign-Info";
            r = crypto_srp_hkdf(
                context->server->pairing_context->srp,
                (byte *)salt3, sizeof(salt3)-1,
                (byte *)info3, sizeof(info3)-1,
                accessory_info, &accessory_x_size
            );
            if (r) {
                DEBUG("Failed to generate AccessoryX (code %d)", r);

                free(accessory_info);
                free(accessory_public_key);

                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }

            memcpy(accessory_info + accessory_x_size,
                   context->server->accessory_id, accessory_id_size);
            memcpy(accessory_info + accessory_x_size + accessory_id_size,
                   accessory_public_key, accessory_public_key_size);

            DEBUG("Generating accessory signature");
            DEBUG("Free heap: %d", xPortGetFreeHeapSize());
            size_t accessory_signature_size = 0;
            crypto_ed25519_sign(
                context->server->accessory_key,
                accessory_info, accessory_info_size,
                NULL, &accessory_signature_size
            );

            byte *accessory_signature = malloc(accessory_signature_size);
            r = crypto_ed25519_sign(
                context->server->accessory_key,
                accessory_info, accessory_info_size,
                accessory_signature, &accessory_signature_size
            );
            if (r) {
                DEBUG("Failed to generate accessory signature (code %d)", r);

                free(accessory_signature);
                free(accessory_public_key);
                free(accessory_info);

                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }

            free(accessory_info);

            tlv_values_t *response_message = tlv_new();
            tlv_add_value(response_message, TLVType_Identifier,
                          (byte *)context->server->accessory_id, accessory_id_size);
            tlv_add_value(response_message, TLVType_PublicKey,
                          accessory_public_key, accessory_public_key_size);
            tlv_add_value(response_message, TLVType_Signature,
                          accessory_signature, accessory_signature_size);

            free(accessory_public_key);
            free(accessory_signature);

            size_t response_data_size = 0;
            tlv_debug(response_message);

            tlv_format(response_message, NULL, &response_data_size);

            byte *response_data = malloc(response_data_size);
            r = tlv_format(response_message, response_data, &response_data_size);
            if (r) {
                DEBUG("Failed to format TLV response (code %d)", r);

                free(response_data);
                tlv_free(response_message);

                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }

            tlv_free(response_message);

            DEBUG("Encrypting response");
            size_t encrypted_response_data_size = 0;
            crypto_chacha20poly1305_encrypt(
                shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg06", NULL, 0,
                response_data, response_data_size,
                NULL, &encrypted_response_data_size
            );

            byte *encrypted_response_data = malloc(encrypted_response_data_size);
            r = crypto_chacha20poly1305_encrypt(
                shared_secret, (byte *)"\x0\x0\x0\x0PS-Msg06", NULL, 0,
                response_data, response_data_size,
                encrypted_response_data, &encrypted_response_data_size
            );

            free(response_data);

            if (r) {
                DEBUG("Failed to encrypt response data (code %d)", r);

                free(encrypted_response_data);

                send_tlv_error_response(context, 6, TLVError_Unknown);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 6);
            tlv_add_value(response, TLVType_EncryptedData,
                          encrypted_response_data, encrypted_response_data_size);

            free(encrypted_response_data);

            send_tlv_response(context, response);

            pairing_context_free(context->server->pairing_context);
            context->server->pairing_context = NULL;

            context->server->paired = 1;

            break;
        }
        default: {
            DEBUG("Unknown state: %d",
                  tlv_get_integer_value(message, TLVType_State, -1));
        }
    }

    tlv_free(message);
}

void homekit_server_on_pair_verify(client_context_t *context, const byte *data, size_t size) {
    DEBUG("HomeKit Pair Verify");
    DEBUG("Free heap: %d", xPortGetFreeHeapSize());

    tlv_values_t *message = tlv_new();
    tlv_parse(data, size, message);

    tlv_debug(message);

    int r;

    switch(tlv_get_integer_value(message, TLVType_State, -1)) {
        case 1: {
            DEBUG("Pair Verify Step 1/2");

            DEBUG("Importing device Curve25519 public key");
            tlv_t *tlv_device_public_key = tlv_get_value(message, TLVType_PublicKey);
            if (!tlv_device_public_key) {
                DEBUG("Device Curve25519 public key not found");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            curve25519_key *device_key = crypto_curve25519_new();
            r = crypto_curve25519_import_public(
                device_key,
                tlv_device_public_key->value, tlv_device_public_key->size
            );
            if (r) {
                DEBUG("Failed to import device Curve25519 public key (code %d)", r);
                crypto_curve25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            DEBUG("Generating accessory Curve25519 key");
            curve25519_key *my_key = crypto_curve25519_generate();
            if (!my_key) {
                DEBUG("Failed to generate accessory Curve25519 key");
                crypto_curve25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            DEBUG("Exporting accessory Curve25519 public key");
            size_t my_key_public_size = 0;
            crypto_curve25519_export_public(my_key, NULL, &my_key_public_size);

            byte *my_key_public = malloc(my_key_public_size);
            r = crypto_curve25519_export_public(my_key, my_key_public, &my_key_public_size);
            if (r) {
                DEBUG("Failed to export accessory Curve25519 public key (code %d)", r);
                free(my_key_public);
                crypto_curve25519_free(my_key);
                crypto_curve25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            DEBUG("Generating Curve25519 shared secret");
            size_t shared_secret_size = 0;
            crypto_curve25519_shared_secret(my_key, device_key, NULL, &shared_secret_size);

            byte *shared_secret = malloc(shared_secret_size);
            r = crypto_curve25519_shared_secret(my_key, device_key, shared_secret, &shared_secret_size);
            crypto_curve25519_free(my_key);
            crypto_curve25519_free(device_key);

            if (r) {
                DEBUG("Failed to generate Curve25519 shared secret (code %d)", r);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            DEBUG("Generating signature");
            size_t accessory_id_size = strlen(context->server->accessory_id);
            size_t accessory_info_size = my_key_public_size + accessory_id_size + tlv_device_public_key->size;

            byte *accessory_info = malloc(accessory_info_size);
            memcpy(accessory_info,
                   my_key_public, my_key_public_size);
            memcpy(accessory_info + my_key_public_size,
                   context->server->accessory_id, accessory_id_size);
            memcpy(accessory_info + my_key_public_size + accessory_id_size,
                   tlv_device_public_key->value, tlv_device_public_key->size);

            size_t accessory_signature_size = 0;
            crypto_ed25519_sign(
                context->server->accessory_key,
                accessory_info, accessory_info_size,
                NULL, &accessory_signature_size
            );

            byte *accessory_signature = malloc(accessory_signature_size);
            r = crypto_ed25519_sign(
                context->server->accessory_key,
                accessory_info, accessory_info_size,
                accessory_signature, &accessory_signature_size
            );
            free(accessory_info);
            if (r) {
                DEBUG("Failed to generate signature (code %d)", r);
                free(accessory_signature);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            tlv_values_t *sub_response = tlv_new();
            tlv_add_value(sub_response, TLVType_Identifier,
                          (const byte *)context->server->accessory_id, accessory_id_size);
            tlv_add_value(sub_response, TLVType_Signature,
                          accessory_signature, accessory_signature_size);

            free(accessory_signature);

            size_t sub_response_data_size = 0;
            tlv_format(sub_response, NULL, &sub_response_data_size);

            byte *sub_response_data = malloc(sub_response_data_size);
            r = tlv_format(sub_response, sub_response_data, &sub_response_data_size);
            tlv_free(sub_response);

            if (r) {
                DEBUG("Failed to format sub-TLV message (code %d)", r);
                free(sub_response_data);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            DEBUG("Generating proof");
            size_t session_key_size = 0;
            const byte salt[] = "Pair-Verify-Encrypt-Salt";
            const byte info[] = "Pair-Verify-Encrypt-Info";
            crypto_hkdf(
                shared_secret, shared_secret_size,
                salt, sizeof(salt)-1,
                info, sizeof(info)-1,
                NULL, &session_key_size
            );

            byte *session_key = malloc(session_key_size);
            r = crypto_hkdf(
                shared_secret, shared_secret_size,
                salt, sizeof(salt)-1,
                info, sizeof(info)-1,
                session_key, &session_key_size
            );
            if (r) {
                DEBUG("Failed to derive session key (code %d)", r);
                free(session_key);
                free(sub_response_data);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            DEBUG("Encrypting response");
            size_t encrypted_response_data_size = 0;
            crypto_chacha20poly1305_encrypt(
                session_key, (byte *)"\x0\x0\x0\x0PV-Msg02", NULL, 0,
                sub_response_data, sub_response_data_size,
                NULL, &encrypted_response_data_size
            );

            byte *encrypted_response_data = malloc(encrypted_response_data_size);
            r = crypto_chacha20poly1305_encrypt(
                session_key, (byte *)"\x0\x0\x0\x0PV-Msg02", NULL, 0,
                sub_response_data, sub_response_data_size,
                encrypted_response_data, &encrypted_response_data_size
            );
            free(sub_response_data);

            if (r) {
                DEBUG("Failed to encrypt sub response data (code %d)", r);
                free(encrypted_response_data);
                free(session_key);
                free(shared_secret);
                free(my_key_public);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 2);
            tlv_add_value(response, TLVType_PublicKey,
                          my_key_public, my_key_public_size);
            tlv_add_value(response, TLVType_EncryptedData,
                          encrypted_response_data, encrypted_response_data_size);

            free(encrypted_response_data);

            send_tlv_response(context, response);

            tlv_free(response);

            if (context->verify_context)
                pair_verify_context_free(context->verify_context);

            context->verify_context = pair_verify_context_new();
            context->verify_context->secret = shared_secret;
            context->verify_context->secret_size = shared_secret_size;

            context->verify_context->session_key = session_key;
            context->verify_context->session_key_size = session_key_size;

            context->verify_context->accessory_public_key = my_key_public;
            context->verify_context->accessory_public_key_size = my_key_public_size;

            context->verify_context->device_public_key = malloc(tlv_device_public_key->size);
            memcpy(context->verify_context->device_public_key,
                   tlv_device_public_key->value, tlv_device_public_key->size);
            context->verify_context->device_public_key_size = tlv_device_public_key->size;

            break;
        }
        case 3: {
            DEBUG("Pair Verify Step 2/2");

            if (!context->verify_context) {
                DEBUG("Failed to verify: no state 1 data");
                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_encrypted_data = tlv_get_value(message, TLVType_EncryptedData);
            if (!tlv_encrypted_data) {
                DEBUG("Failed to verify: no encrypted data");

                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            DEBUG("Decrypting payload");
            size_t decrypted_data_size = 0;
            crypto_chacha20poly1305_decrypt(
                context->verify_context->session_key, (byte *)"\x0\x0\x0\x0PV-Msg03", NULL, 0,
                tlv_encrypted_data->value, tlv_encrypted_data->size,
                NULL, &decrypted_data_size
            );

            byte *decrypted_data = malloc(decrypted_data_size);
            r = crypto_chacha20poly1305_decrypt(
                context->verify_context->session_key, (byte *)"\x0\x0\x0\x0PV-Msg03", NULL, 0,
                tlv_encrypted_data->value, tlv_encrypted_data->size,
                decrypted_data, &decrypted_data_size
            );
            if (r) {
                DEBUG("Failed to decrypt data (code %d)", r);

                free(decrypted_data);
                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_values_t *decrypted_message = tlv_new();
            r = tlv_parse(decrypted_data, decrypted_data_size, decrypted_message);
            free(decrypted_data);

            if (r) {
                DEBUG("Failed to parse decrypted TLV (code %d)", r);

                tlv_free(decrypted_message);
                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_id = tlv_get_value(decrypted_message, TLVType_Identifier);
            if (!tlv_device_id) {
                DEBUG("Invalid encrypted payload: no device identifier");

                tlv_free(decrypted_message);
                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_signature = tlv_get_value(decrypted_message, TLVType_Signature);
            if (!tlv_device_signature) {
                DEBUG("Invalid encrypted payload: no device identifier");

                tlv_free(decrypted_message);
                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            char *device_id = strndup((const char *)tlv_device_id->value, tlv_device_id->size);
            DEBUG("Searching pairing for %s", device_id);
            pairing_t *pairing = homekit_storage_find_pairing(device_id);
            if (!pairing) {
                DEBUG("No pairing for %s found", device_id);

                free(device_id);
                tlv_free(decrypted_message);
                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }
            free(device_id);

            byte permissions = pairing->permissions;
            int pairing_id = pairing->id;

            size_t device_info_size =
                context->verify_context->device_public_key_size +
                context->verify_context->accessory_public_key_size +
                tlv_device_id->size;

            byte *device_info = malloc(device_info_size);
            memcpy(device_info,
                   context->verify_context->device_public_key, context->verify_context->device_public_key_size);
            memcpy(device_info + context->verify_context->device_public_key_size,
                   tlv_device_id->value, tlv_device_id->size);
            memcpy(device_info + context->verify_context->device_public_key_size + tlv_device_id->size,
                   context->verify_context->accessory_public_key, context->verify_context->accessory_public_key_size);

            DEBUG("Verifying device signature");
            r = crypto_ed25519_verify(
                pairing->device_key,
                device_info, device_info_size,
                tlv_device_signature->value, tlv_device_signature->size
            );
            free(device_info);
            pairing_free(pairing);
            tlv_free(decrypted_message);

            if (r) {
                DEBUG("Failed to verify device signature (code %d)", r);

                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Authentication);
                break;
            }

            const byte salt[] = "Control-Salt";
            // TODO: generate read & write keys
            size_t read_key_size = 32;
            context->read_key = malloc(read_key_size);
            const byte read_info[] = "Control-Read-Encryption-Key";
            r = crypto_hkdf(
                context->verify_context->secret, context->verify_context->secret_size,
                salt, sizeof(salt)-1,
                read_info, sizeof(read_info)-1,
                context->read_key, &read_key_size
            );

            if (r) {
                DEBUG("Failed to derive read encryption key (code %d)", r);

                free(context->read_key);
                context->read_key = NULL;
                pair_verify_context_free(context->verify_context);
                context->verify_context = NULL;

                send_tlv_error_response(context, 4, TLVError_Unknown);
                break;
            }

            size_t write_key_size = 32;
            context->write_key = malloc(write_key_size);
            const byte write_info[] = "Control-Write-Encryption-Key";
            r = crypto_hkdf(
                context->verify_context->secret, context->verify_context->secret_size,
                salt, sizeof(salt)-1,
                write_info, sizeof(write_info)-1,
                context->write_key, &write_key_size
            );

            pair_verify_context_free(context->verify_context);
            context->verify_context = NULL;

            if (r) {
                DEBUG("Failed to derive write encryption key (code %d)", r);

                free(context->write_key);
                context->write_key = NULL;
                free(context->read_key);
                context->read_key = NULL;

                send_tlv_error_response(context, 4, TLVError_Unknown);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 4);

            send_tlv_response(context, response);

            tlv_free(response);

            context->pairing_id = pairing_id;
            context->permissions = permissions;
            context->encrypted = true;

            break;
        }
        default: {
            DEBUG("Unknown state: %d",
                  tlv_get_integer_value(message, TLVType_State, -1));
        }
    }

    tlv_free(message);
}


void homekit_server_on_get_accessories(client_context_t *context) {
    DEBUG("HomeKit Get Accessories");
    DEBUG("Free heap: %d", xPortGetFreeHeapSize());

    cJSON *json = cJSON_CreateObject();
    cJSON *j_accessories = cJSON_CreateArray();
    cJSON_AddItemToObject(json, "accessories", j_accessories);

    for (homekit_accessory_t **accessory_it = context->server->accessories; *accessory_it; accessory_it++) {
        homekit_accessory_t *accessory = *accessory_it;

        cJSON *j_accessory = cJSON_CreateObject();
        cJSON_AddItemToArray(j_accessories, j_accessory);

        cJSON_AddNumberToObject(j_accessory, "aid", accessory->id);
        cJSON *j_services = cJSON_CreateArray();
        cJSON_AddItemToObject(j_accessory, "services", j_services);

        for (homekit_service_t **service_it = accessory->services; *service_it; service_it++) {
            homekit_service_t *service = *service_it;

            cJSON *j_service = cJSON_CreateObject();
            cJSON_AddItemToArray(j_services, j_service);

            cJSON_AddNumberToObject(j_service, "iid", service->id);
            cJSON_AddStringToObject(j_service, "type", service->type);
            cJSON_AddBoolToObject(j_service, "hidden", service->hidden);
            cJSON_AddBoolToObject(j_service, "primary", service->primary);
            // TODO: linked services
            // cJSON_AddItemToObject(j_service, "linked", cJSON_CreateArray());

            cJSON *j_characteristics = cJSON_CreateArray();
            cJSON_AddItemToObject(j_service, "characteristics", j_characteristics);

            for (homekit_characteristic_t **ch_it = service->characteristics; *ch_it; ch_it++) {
                homekit_characteristic_t *ch = *ch_it;

                cJSON_AddItemToArray(
                    j_characteristics,
                    characteristic_to_json(
                        context,
                        ch,
                          characteristic_format_type
                        | characteristic_format_meta
                        | characteristic_format_perms
                        | characteristic_format_events
                    )
                );
            }
        }
    }

    send_json_response(context, 200, json);

    cJSON_Delete(json);
}

void homekit_server_on_get_characteristics(client_context_t *context) {
    DEBUG("HomeKit Get Characteristics");
    DEBUG("Free heap: %d", xPortGetFreeHeapSize());

    query_param_t *qp = context->endpoint_params;
    while (qp) {
        DEBUG("Query paramter %s = %s", qp->name, qp->value);
        qp = qp->next;
    }

    query_param_t *id_param = query_params_find(context->endpoint_params, "id");
    if (!id_param) {
        DEBUG("Invalid get characteristics request: missing ID parameter");
        // TODO: respond with a Bad Request
        return;
    }
    char *id = strdup(id_param->value);

    bool bool_endpoint_param(const char *name) {
        query_param_t *param = query_params_find(context->endpoint_params, name);
        return param && param->value && !strcmp(param->value, "1");
    }

    characteristic_format_t format = 0;
    if (bool_endpoint_param("meta")) 
        format |= characteristic_format_meta;

    if (bool_endpoint_param("perms"))
        format |= characteristic_format_perms;

    if (bool_endpoint_param("type"))
        format |= characteristic_format_type;

    if (bool_endpoint_param("ev"))
        format |= characteristic_format_events;

    bool success = true;

    cJSON *json = cJSON_CreateObject();
    cJSON *characteristics = cJSON_CreateArray();
    cJSON_AddItemToObject(json, "characteristics", characteristics);

    cJSON *characteristic_error(int aid, int iid, int status) {
        cJSON *json = cJSON_CreateObject();
        cJSON_AddNumberToObject(json, "aid", aid);
        cJSON_AddNumberToObject(json, "iid", iid);
        cJSON_AddNumberToObject(json, "status", status);
        return json;
    }

    char *ch_id;
    while ((ch_id = strsep(&id, ","))) {
        char *dot = strstr(ch_id, ".");
        if (!dot) {
            // TODO: respond with Bad Request
            return;
        }

        *dot = 0;
        int aid = atoi(ch_id);
        int iid = atoi(dot+1);

        DEBUG("Requested characteristic info for %d.%d", aid, iid);
        homekit_characteristic_t *ch = homekit_characteristic_find_by_id(context->server->accessories, aid, iid);
        if (!ch) {
            cJSON_AddItemToArray(
                characteristics,
                characteristic_error(aid, iid, HAPStatus_NoResource)
            );
            success = false;
            continue;
        }

        if (!(ch->permissions & homekit_permissions_paired_read)) {
            cJSON_AddItemToArray(
                characteristics,
                characteristic_error(aid, iid, HAPStatus_WriteOnly)
            );
            success = false;
            continue;
        }

        cJSON *ch_json = characteristic_to_json(context, ch, format);
        cJSON_AddItemToArray(characteristics, ch_json);
    }

    if (!success) {
        cJSON *ch_json;
        cJSON_ArrayForEach(ch_json, characteristics) {
            if (cJSON_GetObjectItem(ch_json, "status"))
                continue;

            cJSON_AddNumberToObject(ch_json, "status", HAPStatus_Success);
        }
    }

    send_json_response(context, success ? 200 : 207, json);

    cJSON_Delete(json);
}

void homekit_server_on_update_characteristics(client_context_t *context, const byte *data, size_t size) {
    DEBUG("HomeKit Update Characteristics");

    char *json_string = strndup((char *)data, size);
    cJSON *json = cJSON_Parse(json_string);
    free(json_string);

    if (!json) {
        DEBUG("Failed to parse request JSON");
        send_json_error_response(context, 400, HAPStatus_InvalidValue);
        return;
    }

    DEBUG("Parsed JSON payload");

    cJSON *characteristics = cJSON_GetObjectItem(json, "characteristics");
    if (!characteristics) {
        DEBUG("Failed to parse request: no \"characteristics\" field");
        send_json_error_response(context, 400, HAPStatus_InvalidValue);
        return;
    }
    if (characteristics->type != cJSON_Array) {
        DEBUG("Failed to parse request: \"characteristics\" field is not an list");
        send_json_error_response(context, 400, HAPStatus_InvalidValue);
        return;
    }

    DEBUG("Got \"characteristics\" field");

    HAPStatus process_characteristics_update(const cJSON *j_ch) {
        cJSON *j_aid = cJSON_GetObjectItem(j_ch, "aid");
        if (!j_aid) {
            DEBUG("Failed to process request: no \"aid\" field");
            return HAPStatus_NoResource;
        }
        if (j_aid->type != cJSON_Number) {
            DEBUG("Failed to process request: \"aid\" field is not a number");
            return HAPStatus_NoResource;
        }

        cJSON *j_iid = cJSON_GetObjectItem(j_ch, "iid");
        if (!j_iid) {
            DEBUG("Failed to process request: no \"iid\" field");
            return HAPStatus_NoResource;
        }
        if (j_iid->type != cJSON_Number) {
            DEBUG("Failed to process request: \"iid\" field is not a number");
            return HAPStatus_NoResource;
        }

        int aid = j_aid->valueint;
        int iid = j_iid->valueint;

        homekit_characteristic_t *ch = homekit_characteristic_find_by_id(
            context->server->accessories, aid, iid
        );
        if (!ch) {
            DEBUG("Failed to process request to update %d.%d: "
                  "no such characteristic", aid, iid);
            return HAPStatus_NoResource;
        }

        cJSON *j_value = cJSON_GetObjectItem(j_ch, "value");
        if (j_value) {
            if (!(ch->permissions & homekit_permissions_paired_write)) {
                DEBUG("Failed to update %d.%d: no write permission", aid, iid);
                return HAPStatus_ReadOnly;
            }

            switch (ch->format) {
                case homekit_format_bool: {
                    bool value = false;
                    if (j_value->type == cJSON_True) {
                        value = true;
                    } else if (j_value->type == cJSON_False) {
                        value = false;
                    } else if (j_value->type == cJSON_Number &&
                            (j_value->valueint == 0 || j_value->valueint == 1)) {
                        value = j_value->valueint == 1;
                    } else {
                        DEBUG("Failed to update %d.%d: value is not a boolean or 0/1", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    if (ch->setter) {
                        ((bool_setter)ch->setter)(value);
                    } else {
                        ch->bool_value = value;
                    }
                    break;
                }
                case homekit_format_uint8:
                case homekit_format_uint16:
                case homekit_format_uint32:
                case homekit_format_uint64:
                case homekit_format_int: {
                    if (j_value->type != cJSON_Number) {
                        DEBUG("Failed to update %d.%d: value is not a number", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    unsigned long long min_value = 0;
                    unsigned long long max_value = 0;

                    switch (ch->format) {
                        case homekit_format_uint8: {
                            min_value = 0;
                            max_value = 255;
                            break;
                        }
                        case homekit_format_uint16: {
                            min_value = 0;
                            max_value = 65535;
                            break;
                        }
                        case homekit_format_uint32: {
                            min_value = 0;
                            max_value = 4294967295;
                            break;
                        }
                        case homekit_format_uint64: {
                            min_value = 0;
                            max_value = 18446744073709551615ULL;
                            break;
                        }
                        case homekit_format_int: {
                            min_value = -2147483648;
                            max_value = 2147483647;
                            break;
                        }
                        default: {
                            // Impossible, keeping to make compiler happy
                            break;
                        }
                    }

                    if (ch->min_value)
                        min_value = (int)*ch->min_value;
                    if (ch->max_value)
                        max_value = (int)*ch->max_value;

                    int value = j_value->valueint;
                    if (value < min_value || value > max_value) {
                        DEBUG("Failed to update %d.%d: value is not in range", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    if (ch->setter) {
                        ((int_setter)ch->setter)(value);
                    } else {
                        ch->int_value = value;
                    }
                    break;
                }
                case homekit_format_float: {
                    if (j_value->type != cJSON_Number) {
                        DEBUG("Failed to update %d.%d: value is not a number", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    float value = j_value->valuedouble;
                    if ((ch->min_value && value < *ch->min_value) ||
                            (ch->max_value && value > *ch->max_value)) {
                        DEBUG("Failed to update %d.%d: value is not in range", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    if (ch->setter) {
                        ((float_setter)ch->setter)(value);
                    } else {
                        ch->float_value = value;
                    }
                    break;
                }
                case homekit_format_string: {
                    if (j_value->type != cJSON_String) {
                        DEBUG("Failed to update %d.%d: value is not a string", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    int max_len = (ch->max_len) ? *ch->max_len : 64;

                    char *value = j_value->valuestring;
                    if (strlen(value) > max_len) {
                        DEBUG("Failed to update %d.%d: value is too long", aid, iid);
                        return HAPStatus_InvalidValue;
                    }

                    if (ch->setter) {
                        ((string_setter)ch->setter)(value);
                    } else {
                        if (ch->string_value) {
                            free(ch->string_value);
                        }
                        ch->string_value = strdup(value);
                    }
                    break;
                }
                case homekit_format_tlv: {
                    // TODO:
                    break;
                }
                case homekit_format_data: {
                    // TODO:
                    break;
                }
            }
        }

        cJSON *j_events = cJSON_GetObjectItem(j_ch, "ev");
        if (j_events) {
            if (!(ch->permissions && homekit_permissions_notify)) {
                DEBUG("Failed to set notification state for %d.%d: "
                      "notifications are not supported", aid, iid);
                return HAPStatus_NotificationsUnsupported;
            }

            if ((j_events->type != cJSON_True) && (j_events->type != cJSON_False)) {
                DEBUG("Failed to set notification state for %d.%d: "
                      "invalid state value", aid, iid);
            }

            if (j_events->type == cJSON_True) {
                homekit_characteristic_add_notify_callback(ch, client_notify_characteristic, context);
            } else {
                homekit_characteristic_remove_notify_callback(ch, client_notify_characteristic, context);
            }
        }

        return HAPStatus_Success;
    }

    cJSON *result_characteristics = cJSON_CreateArray();

    bool has_errors = false;
    for (int i=0; i < cJSON_GetArraySize(characteristics); i++) {
        cJSON *j_ch = cJSON_GetArrayItem(characteristics, i);

        char *s = cJSON_Print(j_ch);
        DEBUG("Processing element %s", s);
        free(s);

        HAPStatus status = process_characteristics_update(j_ch);

        if (status != HAPStatus_Success)
            has_errors = true;

        cJSON *j_status = cJSON_CreateObject();
        cJSON_AddItemReferenceToObject(j_status, "aid", cJSON_GetObjectItem(j_ch, "aid"));
        cJSON_AddItemReferenceToObject(j_status, "iid", cJSON_GetObjectItem(j_ch, "iid"));
        cJSON_AddNumberToObject(j_status, "status", status);
        cJSON_AddItemToArray(result_characteristics, j_status);
    }

    DEBUG("Finished processing payload");

    if (has_errors) {
        DEBUG("There were processing errors, sending Multi-Status response");

        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "characteristics", result_characteristics);
        send_json_response(context, 207, result);
        cJSON_Delete(result);
    } else {
        DEBUG("There were no processing errors, sending No Content response");

        cJSON_Delete(result_characteristics);
        send_204_response(context);
    }

    cJSON_Delete(json);
}

void homekit_server_on_pairings(client_context_t *context, const byte *data, size_t size) {
    DEBUG("HomeKit Pairings");
    DEBUG("Free heap: %d", xPortGetFreeHeapSize());

    tlv_values_t *message = tlv_new();
    tlv_parse(data, size, message);

    tlv_debug(message);

    int r;

    if (tlv_get_integer_value(message, TLVType_State, -1) != 1) {
        send_tlv_error_response(context, 2, TLVError_Unknown);
        tlv_free(message);
        return;
    }

    switch(tlv_get_integer_value(message, TLVType_Method, -1)) {
        case TLVMethod_AddPairing: {
            DEBUG("Got add pairing request");

            if (!(context->permissions & 1)) {
                DEBUG("Refusing to add pairing to non-admin controller");
                send_tlv_error_response(context, 2, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_identifier = tlv_get_value(message, TLVType_Identifier);
            if (!tlv_device_identifier) {
                DEBUG("Invalid add pairing request: no device identifier");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            tlv_t *tlv_device_public_key = tlv_get_value(message, TLVType_PublicKey);
            if (!tlv_device_public_key) {
                DEBUG("Invalid add pairing request: no device public key");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }
            int device_permissions = tlv_get_integer_value(message, TLVType_Permissions, -1);
            if (device_permissions == -1) {
                DEBUG("Invalid add pairing request: no device permissions");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            ed25519_key *device_key = crypto_ed25519_new();
            r = crypto_ed25519_import_public_key(
                device_key, tlv_device_public_key->value, tlv_device_public_key->size
            );
            if (r) {
                DEBUG("Failed to import device public key");
                crypto_ed25519_free(device_key);
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            char *device_identifier = strndup(
                (const char *)tlv_device_identifier->value,
                tlv_device_identifier->size
            );

            pairing_t *pairing = homekit_storage_find_pairing(device_identifier);
            if (pairing) {
                size_t pairing_public_key_size = 0;
                crypto_ed25519_export_public_key(pairing->device_key, NULL, &pairing_public_key_size);

                byte *pairing_public_key = malloc(pairing_public_key_size);
                r = crypto_ed25519_export_public_key(pairing->device_key, pairing_public_key, &pairing_public_key_size);
                if (r) {
                    DEBUG("Failed to add pairing: error exporting pairing public key (code %d)", r);
                    free(pairing_public_key);
                    pairing_free(pairing);
                    free(device_identifier);
                    crypto_ed25519_free(device_key);
                    send_tlv_error_response(context, 2, TLVError_Unknown);
                }

                pairing_free(pairing);

                if (pairing_public_key_size != tlv_device_public_key->size ||
                        memcmp(tlv_device_public_key->value, pairing_public_key, pairing_public_key_size)) {
                    DEBUG("Failed to add pairing: pairing public key differs from given one");
                    free(pairing_public_key);
                    free(device_identifier);
                    crypto_ed25519_free(device_key);
                    send_tlv_error_response(context, 2, TLVError_Unknown);
                }

                free(pairing_public_key);

                r = homekit_storage_update_pairing(device_identifier, device_permissions);
                if (r) {
                    DEBUG("Failed to add pairing: storage error (code %d)", r);
                    free(device_identifier);
                    crypto_ed25519_free(device_key);
                    send_tlv_error_response(context, 2, TLVError_Unknown);
                    break;
                }
            } else {
                if (!homekit_storage_can_add_pairing()) {
                    DEBUG("Failed to add pairing: max peers");
                    free(device_identifier);
                    crypto_ed25519_free(device_key);
                    send_tlv_error_response(context, 2, TLVError_MaxPeers);
                    break;
                }

                r = homekit_storage_add_pairing(
                    device_identifier, device_key, device_permissions
                );
                if (r) {
                    DEBUG("Failed to add pairing: storage error (code %d)", r);
                    free(device_identifier);
                    crypto_ed25519_free(device_key);
                    send_tlv_error_response(context, 2, TLVError_Unknown);
                    break;
                }
            }

            free(device_identifier);
            crypto_ed25519_free(device_key);

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 2);

            send_tlv_response(context, response);

            tlv_free(response);

            break;
        }
        case TLVMethod_RemovePairing: {
            DEBUG("Got remove pairing request");

            if (!(context->permissions & 1)) {
                DEBUG("Refusing to remove pairing to non-admin controller");
                send_tlv_error_response(context, 2, TLVError_Authentication);
                break;
            }

            tlv_t *tlv_device_identifier = tlv_get_value(message, TLVType_Identifier);
            if (!tlv_device_identifier) {
                DEBUG("Invalid remove pairing request: no device identifier");
                send_tlv_error_response(context, 2, TLVError_Unknown);
                break;
            }

            char *device_identifier = strndup(
                (const char *)tlv_device_identifier->value,
                tlv_device_identifier->size
            );

            pairing_t *pairing = homekit_storage_find_pairing(device_identifier);
            free(device_identifier);

            if (pairing) {
                pairing_free(pairing);

                r = homekit_storage_remove_pairing(device_identifier);
                if (r) {
                    DEBUG("Failed to remove pairing: storage error (code %d)", r);
                    send_tlv_error_response(context, 2, TLVError_Unknown);
                    break;
                }

                client_context_t *c = context->server->clients;
                while (c) {
                    if (c->pairing_id == pairing->id)
                        c->disconnect = true;
                    c = c->next;
                }
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 2);

            send_tlv_response(context, response);

            tlv_free(response);
            break;
        }
        case TLVMethod_ListPairings: {
            DEBUG("Got list pairings request");

            if (!(context->permissions & 1)) {
                DEBUG("Refusing to list pairings to non-admin controller");
                send_tlv_error_response(context, 2, TLVError_Authentication);
                break;
            }

            tlv_values_t *response = tlv_new();
            tlv_add_integer_value(response, TLVType_State, 2);

            bool first = true;

            pairing_iterator_t *it = homekit_storage_pairing_iterator();
            pairing_t *pairing = NULL;

            size_t public_key_size = 32;
            byte *public_key = malloc(public_key_size);

            while ((pairing = homekit_storage_next_pairing(it))) {
                if (!first) {
                    tlv_add_value(response, TLVType_Separator, NULL, 0);
                }
                r = crypto_ed25519_export_public_key(pairing->device_key, public_key, &public_key_size);

                tlv_add_string_value(response, TLVType_Identifier, pairing->device_id);
                tlv_add_value(response, TLVType_PublicKey, public_key, public_key_size);
                tlv_add_integer_value(response, TLVType_Permissions, pairing->permissions);

                first = false;

                pairing_free(pairing);
            }

            free(public_key);
            homekit_storage_pairing_iterator_free(it);

            send_tlv_response(context, response);

            tlv_free(response);
            break;
        }
        default: {
            send_tlv_error_response(context, 2, TLVError_Unknown);
            break;
        }
    }

    tlv_free(message);
}

void homekit_server_on_reset(client_context_t *context) {
    DEBUG("HomeKit Reset");

    homekit_storage_init();
    sdk_system_restart();
}


int homekit_server_on_url(http_parser *parser, const char *data, size_t length) {
    client_context_t *context = (client_context_t*) parser->data;

    context->endpoint = HOMEKIT_ENDPOINT_UNKNOWN;
    if (parser->method == HTTP_GET) {
        if (!strncmp(data, "/accessories", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_GET_ACCESSORIES;
        } else {
            static const char url[] = "/characteristics";
            size_t url_len = sizeof(url)-1;

            if (length >= url_len && !strncmp(data, url, url_len) &&
                    (data[url_len] == 0 || data[url_len] == '?'))
            {
                context->endpoint = HOMEKIT_ENDPOINT_GET_CHARACTERISTICS;
                if (data[url_len] == '?') {
                    char *query = strndup(data+url_len+1, length-url_len-1);
                    context->endpoint_params = query_params_parse(query);
                    free(query);
                }
            }
        }
    } else if (parser->method == HTTP_POST) {
        if (!strncmp(data, "/identify", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_IDENTIFY;
        } else if (!strncmp(data, "/pair-setup", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_PAIR_SETUP;
        } else if (!strncmp(data, "/pair-verify", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_PAIR_VERIFY;
        } else if (!strncmp(data, "/pairings", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_PAIRINGS;
        } else if (!strncmp(data, "/reset", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_RESET;
        }
    } else if (parser->method == HTTP_PUT) {
        if (!strncmp(data, "/characteristics", length)) {
            context->endpoint = HOMEKIT_ENDPOINT_UPDATE_CHARACTERISTICS;
        }
    }

    if (context->endpoint == HOMEKIT_ENDPOINT_UNKNOWN) {
        parser->status_code = 404;
        return -1;
    }

    return 0;
}

int homekit_server_on_body(http_parser *parser, const char *data, size_t length) {
    client_context_t *context = parser->data;
    context->body = realloc(context->body, context->body_length + length);
    memcpy(context->body + context->body_length, data, length);
    context->body_length += length;

    return 0;
}

int homekit_server_on_message_complete(http_parser *parser) {
    client_context_t *context = parser->data;

    // if (context->body) {
    //     char *buffer = binary_to_string((const byte *)context->body, context->body_length);
    //     DEBUG("HTTP body: %s", buffer);
    //     free(buffer);
    // }

    switch(context->endpoint) {
        case HOMEKIT_ENDPOINT_PAIR_SETUP: {
            homekit_server_on_pair_setup(context, (const byte *)context->body, context->body_length);
            break;
        }
        case HOMEKIT_ENDPOINT_PAIR_VERIFY: {
            homekit_server_on_pair_verify(context, (const byte *)context->body, context->body_length);
            break;
        }
        case HOMEKIT_ENDPOINT_IDENTIFY: {
            homekit_server_on_identify(context);
            break;
        }
        case HOMEKIT_ENDPOINT_GET_ACCESSORIES: {
            homekit_server_on_get_accessories(context);
            break;
        }
        case HOMEKIT_ENDPOINT_GET_CHARACTERISTICS: {
            homekit_server_on_get_characteristics(context);
            break;
        }
        case HOMEKIT_ENDPOINT_UPDATE_CHARACTERISTICS: {
            homekit_server_on_update_characteristics(context, (const byte *)context->body, context->body_length);
            break;
        }
        case HOMEKIT_ENDPOINT_PAIRINGS: {
            homekit_server_on_pairings(context, (const byte *)context->body, context->body_length);
            break;
        }
        case HOMEKIT_ENDPOINT_RESET: {
            homekit_server_on_reset(context);
            break;
        }
        case HOMEKIT_ENDPOINT_UNKNOWN: {
            // TODO: do not do anything
            break;
        }
    }

    if (context->endpoint_params) {
        query_params_free(context->endpoint_params);
        context->endpoint_params = NULL;
    }

    if (context->body) {
        free(context->body);
        context->body = NULL;
        context->body_length = 0;
    }

    return 0;
}


static http_parser_settings homekit_http_parser_settings = {
    .on_url = homekit_server_on_url,
    .on_body = homekit_server_on_body,
    .on_message_complete = homekit_server_on_message_complete,
};


static void homekit_client_task(void *_context);

static void homekit_pairing_task(void *_context) {
    DEBUG("Starting pairing task");
    DEBUG("Free heap: %d", xPortGetFreeHeapSize());

    client_context_t *context = (client_context_t *)_context;

    http_parser *parser = malloc(sizeof(http_parser));
    http_parser_init(parser, HTTP_REQUEST);

    parser->data = context;

    const size_t data_size = 256;
    byte *data = malloc(data_size);
    for (;;) {
        if (context->disconnect) {
            DEBUG("Client force disconnect");
            break;
        }

        if (context->server->paired) {
            // already paired, run client in a separate task
            xTaskCreate(homekit_client_task, "HomeKit Client", 768, context, 2, NULL);
            context = NULL;
            break;
        }

        int data_len = lwip_read(context->socket, data, data_size);
        if (data_len == 0) {
            DEBUG("Got %d incomming data", data_len);
            // connection closed
            break;
        }

        if (data_len > 0) {
            DEBUG("Got %d incomming data", data_len);
            byte *payload = (byte *)data;
            size_t payload_size = (size_t)data_len;

            http_parser_execute(
                parser, &homekit_http_parser_settings,
                (char *)payload, payload_size
            );

            if (context->encrypted) {
                free(payload);
            }
        }

        vTaskDelay(1);
    }

    free(data);
    free(parser);

    if (context) {
        DEBUG("Closing client connection");

        lwip_close(context->socket);

        if (context->server->pairing_context && context->server->pairing_context->client == context) {
            pairing_context_free(context->server->pairing_context);
            context->server->pairing_context = NULL;
        }

        if (context->server->clients == context) {
            context->server->clients = context->next;
        } else {
            client_context_t *c = context->server->clients;
            while (c->next && c->next != context)
                c = c->next;
            if (c->next)
                c->next = c->next->next;
        }

        client_context_free(context);
    }
}


static void homekit_client_task(void *_context) {
    DEBUG("Starting client task");
    DEBUG("Free heap: %d", xPortGetFreeHeapSize());

    client_context_t *context = _context;

    http_parser *parser = malloc(sizeof(http_parser));
    http_parser_init(parser, HTTP_REQUEST);

    parser->data = context;

    size_t data_size = 1024 + 18;
    size_t available = 0;
    byte *data = malloc(data_size);
    for (;;) {
        if (context->disconnect) {
            DEBUG("Client force disconnect");
            break;
        }

        homekit_characteristic_t *ch = NULL;
        while (xQueueReceive(context->event_queue, &ch, 0)) {
            send_characteristic_event(context, ch);
        }

        int data_len = lwip_read(context->socket, data+available, data_size-available);
        if (data_len == 0) {
            DEBUG("Got %d incomming data", data_len);
            // connection closed
            break;
        }

        if (data_len > 0) {
            DEBUG("Got %d incomming data", data_len);
            byte *payload = (byte *)data;
            size_t payload_size = (size_t)data_len;

            byte *decrypted = NULL;
            size_t decrypted_size = 0;

            if (context->encrypted) {
                DEBUG("Decrypting data");

                client_decrypt(context, data, data_len, NULL, &decrypted_size);

                decrypted = malloc(decrypted_size);
                int r = client_decrypt(context, data, data_len, decrypted, &decrypted_size);
                if (r < 0) {
                    DEBUG("Invalid client data");
                    free(decrypted);
                    break;
                }
                available = data_len - r;
                if (r && available) {
                    memmove(data, &data[r], available);
                }
                DEBUG("Decrypted %d bytes, available %d", decrypted_size, available);

                payload = decrypted;
                payload_size = decrypted_size;
                if (payload_size)
                    print_binary("Decrypted data", payload, payload_size);
            } else {
                available = 0;
            }

            http_parser_execute(
                parser, &homekit_http_parser_settings,
                (char *)payload, payload_size
            );

            if (decrypted) {
                free(decrypted);
            }
        }

        vTaskDelay(1);
    }

    DEBUG("Closing client connection");

    lwip_close(context->socket);

    free(data);
    free(parser);
    if (context->server->pairing_context && context->server->pairing_context->client == context) {
        pairing_context_free(context->server->pairing_context);
        context->server->pairing_context = NULL;
    }

    if (context->server->clients == context) {
        context->server->clients = context->next;
    } else {
        client_context_t *c = context->server->clients;
        while (c->next && c->next != context)
            c = c->next;
        if (c->next)
            c->next = c->next->next;
    }

    homekit_accessories_clear_notify_callbacks(
        context->server->accessories,
        client_notify_characteristic,
        context
    );

    client_context_free(context);

    vTaskDelete(NULL);
}


static void run_server(server_t *server)
{
    DEBUG("Staring HTTP server");

    struct sockaddr_in serv_addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);
    bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(listenfd, 10);

    for (;;) {
        int s = accept(listenfd, (struct sockaddr *)NULL, (socklen_t *)NULL);
        if (s >= 0) {
            DEBUG("Got new client connection");
            const struct timeval timeout = { 10, 0 }; /* 10 second timeout */
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            client_context_t *context = client_context_new();
            context->server = server;
            context->socket = s;
            context->next = server->clients;

            server->clients = context;

            if (!server->paired) {
                homekit_pairing_task(context);
            } else {
                xTaskCreate(homekit_client_task, "HomeKit Client", 768, context, 2, NULL);
            }
        }
    }

    server_free(server);
}


void homekit_setup_mdns(server_t *server) {
    DEBUG("Configuring mDNS");

    homekit_accessory_t *accessory = server->accessories[0];
    homekit_characteristic_t *name = homekit_characteristic_find_by_type(server->accessories, 1, HOMEKIT_CHARACTERISTIC_NAME);
    if (!name) {
        DEBUG("Invalid accessory declaration: "
              "no Name characteristic in AccessoryInfo service");
        return;
    }

    mdns_init();

    char txt_rec[128];
    txt_rec[0] = 0;

    char buffer[32];
    // accessory model name (required)
    mdns_txt_add(txt_rec, sizeof(txt_rec), "md", name->string_value);
    // protocol version (required)
    mdns_txt_add(txt_rec, sizeof(txt_rec), "pv", "1.0");
    // device ID (required)
    // should be in format XX:XX:XX:XX:XX:XX, otherwise devices will ignore it
    mdns_txt_add(txt_rec, sizeof(txt_rec), "id", server->accessory_id);
    // current configuration number (required)
    snprintf(buffer, sizeof(buffer), "%d", accessory->config_number);
    mdns_txt_add(txt_rec, sizeof(txt_rec), "c#", buffer);
    // current state number (required)
    mdns_txt_add(txt_rec, sizeof(txt_rec), "s#", "1");
    // feature flags (required if non-zero)
    //   bit 0 - supports HAP pairing. required for all HomeKit accessories
    //   bits 1-7 - reserved
    mdns_txt_add(txt_rec, sizeof(txt_rec), "ff", "0");
    // status flags
    //   bit 0 - not paired
    //   bit 1 - not configured to join WiFi
    //   bit 2 - problem detected on accessory
    //   bits 3-7 - reserved
    mdns_txt_add(txt_rec, sizeof(txt_rec), "sf", (server->paired) ? "0" : "1");
    // accessory category identifier
    snprintf(buffer, sizeof(buffer), "%d", accessory->category);
    mdns_txt_add(txt_rec, sizeof(txt_rec), "ci", buffer);

    mdns_add_facility(name->string_value, "hap", txt_rec, mdns_TCP, PORT, 60);
}

char *homekit_accessory_id_generate() {
    char *accessory_id = malloc(18);

    byte buf[6];
    hwrand_fill(buf, sizeof(buf));
    snprintf(accessory_id, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);

    DEBUG("Generated accessory ID: %s", accessory_id);
    return accessory_id;
}

ed25519_key *homekit_accessory_key_generate() {
    ed25519_key *key = crypto_ed25519_generate();
    if (!key) {
        DEBUG("Failed to generate accessory key");
        return NULL;
    }

    DEBUG("Generated new accessory key");

    return key;
}

void homekit_server_task(void *args) {
    server_t *server = args;
    DEBUG("Starting server");

    int r = homekit_storage_init();

    if (r == 0) {
        server->accessory_id = homekit_storage_load_accessory_id();
        server->accessory_key = homekit_storage_load_accessory_key();
    }
    if (!server->accessory_id || !server->accessory_key) {
        server->accessory_id = homekit_accessory_id_generate();
        homekit_storage_save_accessory_id(server->accessory_id);

        server->accessory_key = homekit_accessory_key_generate();
        homekit_storage_save_accessory_key(server->accessory_key);
    }

    pairing_iterator_t *pairing_it = homekit_storage_pairing_iterator();
    pairing_t *pairing = homekit_storage_next_pairing(pairing_it);
    homekit_storage_pairing_iterator_free(pairing_it);

    if (pairing) {
        pairing_free(pairing);

        server->paired = true;
    }

    DEBUG("Using accessory ID: %s", server->accessory_id);

    homekit_setup_mdns(server);

    run_server(server);
}

void homekit_server_init(homekit_accessory_t **accessories) {
    homekit_accessories_init(accessories);

    server_t *server = server_new();
    server->accessories = accessories;

    xTaskCreate(homekit_server_task, "HomeKit Server", 1700, server, 1, NULL);
}
