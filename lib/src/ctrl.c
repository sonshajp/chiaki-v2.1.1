// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/ctrl.h>
#include <chiaki/session.h>
#include <chiaki/base64.h>
#include <chiaki/http.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

#define SESSION_OSTYPE "Win10.0.0"

#define SESSION_CTRL_PORT 9295

#define CTRL_EXPECT_TIMEOUT 5000

typedef enum ctrl_message_type_t {
	CTRL_MESSAGE_TYPE_SESSION_ID = 0x33,
	CTRL_MESSAGE_TYPE_HEARTBEAT_REQ = 0xfe,
	CTRL_MESSAGE_TYPE_HEARTBEAT_REP = 0x1fe,
	CTRL_MESSAGE_TYPE_LOGIN_PIN_REQ = 0x4,
	CTRL_MESSAGE_TYPE_LOGIN_PIN_REP = 0x8004,
	CTRL_MESSAGE_TYPE_LOGIN = 0x5,
	CTRL_MESSAGE_TYPE_GOTO_BED = 0x50,
	CTRL_MESSAGE_TYPE_KEYBOARD_ENABLE_TOGGLE = 0x20,
	CTRL_MESSAGE_TYPE_KEYBOARD_OPEN = 0x21,
	CTRL_MESSAGE_TYPE_KEYBOARD_CLOSE_REMOTE = 0x22,
	CTRL_MESSAGE_TYPE_KEYBOARD_TEXT_CHANGE_REQ = 0x23,
	CTRL_MESSAGE_TYPE_KEYBOARD_TEXT_CHANGE_RES = 0x24,
	CTRL_MESSAGE_TYPE_KEYBOARD_CLOSE_REQ = 0x25,
} CtrlMessageType;

typedef enum ctrl_login_state_t {
	CTRL_LOGIN_STATE_SUCCESS = 0x0,
	CTRL_LOGIN_STATE_PIN_INCORRECT = 0x1
} CtrlLoginState;

struct chiaki_ctrl_message_queue_t
{
	ChiakiCtrlMessageQueue *next;
	uint16_t type;
	uint8_t *payload;
	size_t payload_size;
};

typedef struct ctrl_keyboard_open_t
{
	uint8_t unk[0x1C];
	uint32_t text_length;
} CtrlKeyboardOpenMessage;

typedef struct ctrl_keyboard_text_request_t
{
	uint32_t counter;
	uint32_t text_length1;
	uint8_t unk1[0x8];
	uint8_t unk2[0x10];
	uint32_t text_length2;
} CtrlKeyboardTextRequestMessage;

typedef struct ctrl_keyboard_text_response_t
{
	uint32_t counter;
	uint32_t unk;
	uint32_t text_length1;
	uint32_t unk2;
	uint8_t unk3[0x10];
	uint32_t unk4;
	uint32_t text_length2;
} CtrlKeyboardTextResponseMessage;

void chiaki_session_send_event(ChiakiSession *session, ChiakiEvent *event);

static void *ctrl_thread_func(void *user);
static ChiakiErrorCode ctrl_message_send(ChiakiCtrl *ctrl, uint16_t type, const uint8_t *payload, size_t payload_size);
static void ctrl_enable_optional_features(ChiakiCtrl *ctrl);
static void ctrl_message_received_session_id(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size);
static void ctrl_message_received_heartbeat_req(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size);
static void ctrl_message_received_login_pin_req(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size);
static void ctrl_message_received_login(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size);
static void ctrl_message_received_keyboard_open(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size);
static void ctrl_message_received_keyboard_close(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size);
static void ctrl_message_received_keyboard_text_change(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size);

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_init(ChiakiCtrl *ctrl, ChiakiSession *session)
{
	ctrl->session = session;

	ctrl->should_stop = false;
	ctrl->login_pin_entered = false;
	ctrl->login_pin_requested = false;
	ctrl->login_pin = NULL;
	ctrl->login_pin_size = 0;
	ctrl->msg_queue = NULL;
	ctrl->keyboard_text_counter = 0;

	ChiakiErrorCode err = chiaki_stop_pipe_init(&ctrl->notif_pipe);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	err = chiaki_mutex_init(&ctrl->notif_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_notif_pipe;

	return err;
	chiaki_mutex_fini(&ctrl->notif_mutex);
error_notif_pipe:
	chiaki_stop_pipe_fini(&ctrl->notif_pipe);
	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_start(ChiakiCtrl *ctrl)
{
	ChiakiErrorCode err = chiaki_thread_create(&ctrl->thread, ctrl_thread_func, ctrl);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	chiaki_thread_set_name(&ctrl->thread, "Chiaki Ctrl");
	return err;
}

CHIAKI_EXPORT void chiaki_ctrl_stop(ChiakiCtrl *ctrl)
{
	ChiakiErrorCode err = chiaki_mutex_lock(&ctrl->notif_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	ctrl->should_stop = true;
	chiaki_stop_pipe_stop(&ctrl->notif_pipe);
	chiaki_mutex_unlock(&ctrl->notif_mutex);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_join(ChiakiCtrl *ctrl)
{
	return chiaki_thread_join(&ctrl->thread, NULL);
}

CHIAKI_EXPORT void chiaki_ctrl_fini(ChiakiCtrl *ctrl)
{
	chiaki_stop_pipe_fini(&ctrl->notif_pipe);
	chiaki_mutex_fini(&ctrl->notif_mutex);
	free(ctrl->login_pin);
}

static void ctrl_message_queue_free(ChiakiCtrlMessageQueue *queue)
{
	free(queue->payload);
	free(queue);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_send_message(ChiakiCtrl *ctrl, uint16_t type, const uint8_t *payload, size_t payload_size)
{
	ChiakiCtrlMessageQueue *queue = CHIAKI_NEW(ChiakiCtrlMessageQueue);
	if(!queue)
		return CHIAKI_ERR_MEMORY;
	queue->next = NULL;
	queue->type = type;
	if(payload)
	{
		queue->payload = malloc(payload_size);
		if(!queue->payload)
		{
			free(queue);
			return CHIAKI_ERR_MEMORY;
		}
		memcpy(queue->payload, payload, payload_size);
		queue->payload_size = payload_size;
	}
	else
	{
		queue->payload = NULL;
		queue->payload_size = 0;
	}
	ChiakiErrorCode err = chiaki_mutex_lock(&ctrl->notif_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	if(!ctrl->msg_queue)
		ctrl->msg_queue = queue;
	else
	{
		ChiakiCtrlMessageQueue *c = ctrl->msg_queue;
		while(c->next)
			c = c->next;
		c->next = queue;
	}
	chiaki_mutex_unlock(&ctrl->notif_mutex);
	chiaki_stop_pipe_stop(&ctrl->notif_pipe);
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_ctrl_set_login_pin(ChiakiCtrl *ctrl, const uint8_t *pin, size_t pin_size)
{
	uint8_t *buf = malloc(pin_size);
	if(!buf)
		return;
	memcpy(buf, pin, pin_size);
	ChiakiErrorCode err = chiaki_mutex_lock(&ctrl->notif_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	if(ctrl->login_pin_entered)
		free(ctrl->login_pin);
	ctrl->login_pin_entered = true;
	ctrl->login_pin = buf;
	ctrl->login_pin_size = pin_size;
	chiaki_stop_pipe_stop(&ctrl->notif_pipe);
	chiaki_mutex_unlock(&ctrl->notif_mutex);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_goto_bed(ChiakiCtrl *ctrl)
{
	return chiaki_ctrl_send_message(ctrl, CTRL_MESSAGE_TYPE_GOTO_BED, NULL, 0);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_keyboard_set_text(ChiakiCtrl *ctrl, const char *text)
{
	const uint32_t length = strlen(text);
	const size_t payload_size = sizeof(CtrlKeyboardTextRequestMessage) + length;

	uint8_t *payload = malloc(payload_size);
	if(!payload)
		return CHIAKI_ERR_MEMORY;
	memset(payload, 0, payload_size);
	memcpy(payload + sizeof(CtrlKeyboardTextRequestMessage), text, length);

	CtrlKeyboardTextRequestMessage *msg = (CtrlKeyboardTextRequestMessage *)payload;
	msg->counter = ntohl(++ctrl->keyboard_text_counter);
	msg->text_length1 = ntohl(length);
	msg->text_length2 = ntohl(length);

	ChiakiErrorCode err;
	err = chiaki_ctrl_send_message(ctrl, CTRL_MESSAGE_TYPE_KEYBOARD_TEXT_CHANGE_REQ, payload, payload_size);

	free(payload);
	return err;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_keyboard_accept(ChiakiCtrl *ctrl)
{
	const uint8_t accept[4] = { 0x00, 0x00, 0x00, 0x00 };
	return chiaki_ctrl_send_message(ctrl, CTRL_MESSAGE_TYPE_KEYBOARD_CLOSE_REQ, accept, 4);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_ctrl_keyboard_reject(ChiakiCtrl *ctrl)
{
	const uint8_t reject[4] = { 0x00, 0x00, 0x00, 0x01 };
	return chiaki_ctrl_send_message(ctrl, CTRL_MESSAGE_TYPE_KEYBOARD_CLOSE_REQ, reject, 4);
}

static ChiakiErrorCode ctrl_connect(ChiakiCtrl *ctrl);
static void ctrl_message_received(ChiakiCtrl *ctrl, uint16_t msg_type, uint8_t *payload, size_t payload_size);

static void ctrl_failed(ChiakiCtrl *ctrl, ChiakiQuitReason reason)
{
	ChiakiErrorCode mutex_err = chiaki_mutex_lock(&ctrl->session->state_mutex);
	assert(mutex_err == CHIAKI_ERR_SUCCESS);
	ctrl->session->quit_reason = reason;
	ctrl->session->ctrl_failed = true;
	chiaki_mutex_unlock(&ctrl->session->state_mutex);
	chiaki_cond_signal(&ctrl->session->state_cond);
}

static void *ctrl_thread_func(void *user)
{
	ChiakiCtrl *ctrl = user;

	ChiakiErrorCode err = chiaki_mutex_lock(&ctrl->notif_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);

	err = ctrl_connect(ctrl);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		ctrl_failed(ctrl, CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED);
		chiaki_mutex_unlock(&ctrl->notif_mutex);
		return NULL;
	}

	CHIAKI_LOGI(ctrl->session->log, "Ctrl connected");

	while(true)
	{
		bool overflow = false;
		while(ctrl->recv_buf_size >= 8)
		{
			uint32_t payload_size = *((uint32_t *)ctrl->recv_buf);
			payload_size = ntohl(payload_size);

			if(ctrl->recv_buf_size < 8 + payload_size)
			{
				if(8 + payload_size > sizeof(ctrl->recv_buf))
				{
					CHIAKI_LOGE(ctrl->session->log, "Ctrl buffer overflow!");
					overflow = true;
				}
				break;
			}

			uint16_t msg_type = *((chiaki_unaligned_uint16_t *)(ctrl->recv_buf + 4));
			msg_type = ntohs(msg_type);

			ctrl_message_received(ctrl, msg_type, ctrl->recv_buf + 8, (size_t)payload_size);
			ctrl->recv_buf_size -= 8 + payload_size;
			if(ctrl->recv_buf_size > 0)
				memmove(ctrl->recv_buf, ctrl->recv_buf + 8 + payload_size, ctrl->recv_buf_size);
		}

		if(overflow)
		{
			ctrl_failed(ctrl, CHIAKI_QUIT_REASON_CTRL_UNKNOWN);
			break;
		}

		chiaki_mutex_unlock(&ctrl->notif_mutex);
		err = chiaki_stop_pipe_select_single(&ctrl->notif_pipe, ctrl->sock, false, UINT64_MAX);
		chiaki_mutex_lock(&ctrl->notif_mutex);

		bool msg_queue_updated = false;
		if(err == CHIAKI_ERR_CANCELED)
		{
			while(ctrl->msg_queue)
			{
				ctrl_message_send(ctrl, ctrl->msg_queue->type, ctrl->msg_queue->payload, ctrl->msg_queue->payload_size);
				ChiakiCtrlMessageQueue *next = ctrl->msg_queue->next;
				ctrl_message_queue_free(ctrl->msg_queue);
				ctrl->msg_queue = next;
				msg_queue_updated = true;
			}

			if(ctrl->login_pin_entered)
			{
				CHIAKI_LOGI(ctrl->session->log, "Ctrl received entered Login PIN, sending to console");
				ctrl_message_send(ctrl, CTRL_MESSAGE_TYPE_LOGIN_PIN_REP, ctrl->login_pin, ctrl->login_pin_size);
				ctrl->login_pin_entered = false;
				free(ctrl->login_pin);
				ctrl->login_pin = NULL;
				ctrl->login_pin_size = 0;
				chiaki_stop_pipe_reset(&ctrl->notif_pipe);
				continue;
			}

			if(ctrl->should_stop)
			{
				CHIAKI_LOGI(ctrl->session->log, "Ctrl requested to stop");
				break;
			}

			if(msg_queue_updated)
				chiaki_stop_pipe_reset(&ctrl->notif_pipe);

			continue;
		}
		else if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(ctrl->session->log, "Ctrl select error: %s", chiaki_error_string(err));
			break;
		}

		int received = recv(ctrl->sock, ctrl->recv_buf + ctrl->recv_buf_size, sizeof(ctrl->recv_buf) - ctrl->recv_buf_size, 0);
		if(received <= 0)
		{
			if(received < 0)
			{
				CHIAKI_LOGE(ctrl->session->log, "Ctrl failed to recv: " CHIAKI_SOCKET_ERROR_FMT, CHIAKI_SOCKET_ERROR_VALUE);
				ctrl_failed(ctrl, CHIAKI_QUIT_REASON_CTRL_UNKNOWN);
			}
			break;
		}

		ctrl->recv_buf_size += received;
	}

	chiaki_mutex_unlock(&ctrl->notif_mutex);

	CHIAKI_SOCKET_CLOSE(ctrl->sock);

	return NULL;
}

static ChiakiErrorCode ctrl_message_send(ChiakiCtrl *ctrl, uint16_t type, const uint8_t *payload, size_t payload_size)
{
	assert(payload_size == 0 || payload);

	CHIAKI_LOGV(ctrl->session->log, "Ctrl sending message type %x, size %llx\n",
			(unsigned int)type, (unsigned long long)payload_size);
	if(payload)
		chiaki_log_hexdump(ctrl->session->log, CHIAKI_LOG_VERBOSE, payload, payload_size);

	uint8_t *enc = NULL;
	if(payload && payload_size)
	{
		enc = malloc(payload_size);
		if(!enc)
			return CHIAKI_ERR_MEMORY;
		ChiakiErrorCode err = chiaki_rpcrypt_encrypt(&ctrl->session->rpcrypt, ctrl->crypt_counter_local++, payload, enc, payload_size);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(ctrl->session->log, "Ctrl failed to encrypt payload");
			free(enc);
			return err;
		}
	}

#ifdef __GNUC__
	__attribute__((aligned(__alignof__(uint32_t))))
#endif
	uint8_t header[8];
	*((uint32_t *)header) = htonl((uint32_t)payload_size);
	*((uint16_t *)(header + 4)) = htons(type);
	*((uint16_t *)(header + 6)) = 0;

	int sent = send(ctrl->sock, header, sizeof(header), 0);
	if(sent < 0)
	{
		CHIAKI_LOGE(ctrl->session->log, "Failed to send Ctrl Message Header");
		return CHIAKI_ERR_NETWORK;
	}

	if(enc)
	{
		sent = send(ctrl->sock, enc, payload_size, 0);
		free(enc);
		if(sent < 0)
		{
			CHIAKI_LOGE(ctrl->session->log, "Failed to send Ctrl Message Payload");
			return CHIAKI_ERR_NETWORK;
		}
	}

	return CHIAKI_ERR_SUCCESS;
}

static void ctrl_message_received(ChiakiCtrl *ctrl, uint16_t msg_type, uint8_t *payload, size_t payload_size)
{
	if(payload_size > 0)
	{
		ChiakiErrorCode err = chiaki_rpcrypt_decrypt(&ctrl->session->rpcrypt, ctrl->crypt_counter_remote++, payload, payload, payload_size);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(ctrl->session->log, "Failed to decrypt payload for Ctrl Message type %#x", msg_type);
			return;
		}
	}

	CHIAKI_LOGV(ctrl->session->log, "Ctrl received message of type %#x, size %#llx", (unsigned int)msg_type, (unsigned long long)payload_size);
	if(payload_size > 0)
		chiaki_log_hexdump(ctrl->session->log, CHIAKI_LOG_VERBOSE, payload, payload_size);

	switch(msg_type)
	{
		case CTRL_MESSAGE_TYPE_SESSION_ID:
			ctrl_message_received_session_id(ctrl, payload, payload_size);
			ctrl_enable_optional_features(ctrl);
			break;
		case CTRL_MESSAGE_TYPE_HEARTBEAT_REQ:
			ctrl_message_received_heartbeat_req(ctrl, payload, payload_size);
			break;
		case CTRL_MESSAGE_TYPE_LOGIN_PIN_REQ:
			ctrl_message_received_login_pin_req(ctrl, payload, payload_size);
			break;
		case CTRL_MESSAGE_TYPE_LOGIN:
			ctrl_message_received_login(ctrl, payload, payload_size);
			break;
		case CTRL_MESSAGE_TYPE_KEYBOARD_OPEN:
			ctrl_message_received_keyboard_open(ctrl, payload, payload_size);
			break;
		case CTRL_MESSAGE_TYPE_KEYBOARD_TEXT_CHANGE_RES:
			ctrl_message_received_keyboard_text_change(ctrl, payload, payload_size);
			break;
		case CTRL_MESSAGE_TYPE_KEYBOARD_CLOSE_REMOTE:
			ctrl_message_received_keyboard_close(ctrl, payload, payload_size);
			break;
      default:
			CHIAKI_LOGW(ctrl->session->log, "Received Ctrl Message with unknown type %#x", msg_type);
			chiaki_log_hexdump(ctrl->session->log, CHIAKI_LOG_WARNING, payload, payload_size);
			break;
	}
}

static void ctrl_enable_optional_features(ChiakiCtrl *ctrl)
{
	if(!ctrl->session->connect_info.enable_keyboard)
		return;
	// TODO: Last byte of pre_enable request is random (?)
	// TODO: Signature ?!
	uint8_t enable = 1;
	uint8_t pre_enable[4] = { 0x00, 0x01, 0x01, 0x80 };
	uint8_t signature[0x10] = { 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x05, 0xAE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	ctrl_message_send(ctrl, 0xD, signature, 0x10);
	ctrl_message_send(ctrl, 0x36, pre_enable, 4);
	ctrl_message_send(ctrl, CTRL_MESSAGE_TYPE_KEYBOARD_ENABLE_TOGGLE, &enable, 1);
	ctrl_message_send(ctrl, 0x36, pre_enable, 4);
}

static void ctrl_message_received_session_id(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size)
{
	if(ctrl->session->ctrl_session_id_received)
	{
		CHIAKI_LOGW(ctrl->session->log, "Received another Session Id Message");
		return;
	}

	if(payload_size < 2)
	{
		CHIAKI_LOGE(ctrl->session->log, "Invalid Session Id \"%s\" received", payload);
		return;
	}

	if(payload[0] != 0x4a)
	{
		CHIAKI_LOGW(ctrl->session->log, "Received presumably invalid Session Id:");
		chiaki_log_hexdump(ctrl->session->log, CHIAKI_LOG_WARNING, payload, payload_size);
	}

	// skip the size
	payload++;
	payload_size--;

	if(payload_size >= CHIAKI_SESSION_ID_SIZE_MAX - 1)
	{
		CHIAKI_LOGE(ctrl->session->log, "Received Session Id is too long");
		return;
	}

	for(uint8_t *cur=payload; cur<payload+payload_size; cur++)
	{
		char c = *cur;
		if(c >= 'a' && c <= 'z')
			continue;
		if(c >= 'A' && c <= 'Z')
			continue;
		if(c >= '0' && c <= '9')
			continue;
		CHIAKI_LOGE(ctrl->session->log, "Ctrl received Session Id contains invalid characters");
		return;
	}

	memcpy(ctrl->session->session_id, payload, payload_size);
	ctrl->session->session_id[payload_size] = '\0';
	chiaki_mutex_lock(&ctrl->session->state_mutex);
	ctrl->session->ctrl_session_id_received = true;
	chiaki_mutex_unlock(&ctrl->session->state_mutex);
	chiaki_cond_signal(&ctrl->session->state_cond);

	CHIAKI_LOGI(ctrl->session->log, "Ctrl received valid Session Id: %s", ctrl->session->session_id);
}

static void ctrl_message_received_heartbeat_req(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size)
{
	if(payload_size != 0)
		CHIAKI_LOGW(ctrl->session->log, "Ctrl received Heartbeat request with non-empty payload");

	CHIAKI_LOGI(ctrl->session->log, "Ctrl received Heartbeat, sending reply");

	ctrl_message_send(ctrl, CTRL_MESSAGE_TYPE_HEARTBEAT_REP, NULL, 0);
}

static void ctrl_message_received_login_pin_req(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size)
{
	if(payload_size != 0)
		CHIAKI_LOGW(ctrl->session->log, "Ctrl received Login PIN request with non-empty payload");

	CHIAKI_LOGI(ctrl->session->log, "Ctrl received Login PIN request");

	ctrl->login_pin_requested = true;

	ChiakiErrorCode err = chiaki_mutex_lock(&ctrl->session->state_mutex);
	assert(err == CHIAKI_ERR_SUCCESS);
	ctrl->session->ctrl_login_pin_requested = true;
	chiaki_mutex_unlock(&ctrl->session->state_mutex);
	chiaki_cond_signal(&ctrl->session->state_cond);
}

static void ctrl_message_received_login(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size)
{
	if(payload_size != 1)
	{
		CHIAKI_LOGW(ctrl->session->log, "Ctrl received Login message with payload of size %#llx", (unsigned long long)payload_size);
		if(payload_size < 1)
			return;
	}

	CtrlLoginState state = payload[0];
	switch(state)
	{
		case CTRL_LOGIN_STATE_SUCCESS:
			CHIAKI_LOGI(ctrl->session->log, "Ctrl received Login message: success");
			ctrl->login_pin_requested = false;
			break;
		case CTRL_LOGIN_STATE_PIN_INCORRECT:
			CHIAKI_LOGI(ctrl->session->log, "Ctrl received Login message: PIN incorrect");
			if(ctrl->login_pin_requested)
			{
				CHIAKI_LOGI(ctrl->session->log, "Ctrl requesting PIN from Session again");
				ChiakiErrorCode err = chiaki_mutex_lock(&ctrl->session->state_mutex);
				assert(err == CHIAKI_ERR_SUCCESS);
				ctrl->session->ctrl_login_pin_requested = true;
				chiaki_mutex_unlock(&ctrl->session->state_mutex);
				chiaki_cond_signal(&ctrl->session->state_cond);
			}
			else
				CHIAKI_LOGW(ctrl->session->log, "Ctrl Login PIN incorrect message, but PIN was not requested");
			break;
		default:
			CHIAKI_LOGI(ctrl->session->log, "Ctrl received Login message with state: %#x", state);
			break;
	}
}

static void ctrl_message_received_keyboard_open(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size)
{
	assert(payload_size >= sizeof(CtrlKeyboardOpenMessage));

	CtrlKeyboardOpenMessage *msg = (CtrlKeyboardOpenMessage *)payload;
	msg->text_length = ntohl(msg->text_length);
	assert(payload_size == sizeof(CtrlKeyboardOpenMessage) + msg->text_length);

	uint8_t *buffer = msg->text_length > 0 ? malloc((size_t)msg->text_length + 1) : NULL;
	if(buffer)
	{
		buffer[msg->text_length] = '\0';
		memcpy(buffer, payload + sizeof(CtrlKeyboardOpenMessage), msg->text_length);
	}

	ChiakiEvent keyboard_event;
	keyboard_event.type = CHIAKI_EVENT_KEYBOARD_OPEN;
	keyboard_event.keyboard.text_str = (const char *)buffer;
	chiaki_session_send_event(ctrl->session, &keyboard_event);

	if(buffer)
		free(buffer);
}

static void ctrl_message_received_keyboard_close(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size)
{
	(void)payload;
	(void)payload_size;

	ChiakiEvent keyboard_event;
	keyboard_event.type = CHIAKI_EVENT_KEYBOARD_REMOTE_CLOSE;
	keyboard_event.keyboard.text_str = NULL;
	chiaki_session_send_event(ctrl->session, &keyboard_event);
}

static void ctrl_message_received_keyboard_text_change(ChiakiCtrl *ctrl, uint8_t *payload, size_t payload_size)
{
	assert(payload_size >= sizeof(CtrlKeyboardTextResponseMessage));

	CtrlKeyboardTextResponseMessage *msg = (CtrlKeyboardTextResponseMessage *)payload;
	msg->text_length1 = ntohl(msg->text_length1);
	assert(payload_size == sizeof(CtrlKeyboardTextResponseMessage) + msg->text_length1);

	uint8_t *buffer = msg->text_length1 > 0 ? malloc((size_t)msg->text_length1 + 1) : NULL;
	if(buffer)
	{
		buffer[msg->text_length1] = '\0';
		memcpy(buffer, payload + sizeof(CtrlKeyboardTextResponseMessage), msg->text_length1);
	}

	ChiakiEvent keyboard_event;
	keyboard_event.type = CHIAKI_EVENT_KEYBOARD_TEXT_CHANGE;
	keyboard_event.keyboard.text_str = (const char *)buffer;
	chiaki_session_send_event(ctrl->session, &keyboard_event);

	if(buffer)
		free(buffer);
}

typedef struct ctrl_response_t
{
	bool server_type_valid;
	uint8_t rp_server_type[0x10];
	bool success;
} CtrlResponse;

static void parse_ctrl_response(CtrlResponse *response, ChiakiHttpResponse *http_response)
{
	memset(response, 0, sizeof(CtrlResponse));

	if(http_response->code != 200)
	{
		response->success = false;
		return;
	}

	response->success = true;
	response->server_type_valid = false;
	for(ChiakiHttpHeader *header=http_response->headers; header; header=header->next)
	{
		if(strcmp(header->key, "RP-Server-Type") == 0)
		{
			size_t server_type_size = sizeof(response->rp_server_type);
			chiaki_base64_decode(header->value, strlen(header->value) + 1, response->rp_server_type, &server_type_size);
			response->server_type_valid = server_type_size == sizeof(response->rp_server_type);
		}
	}
}

static ChiakiErrorCode ctrl_connect(ChiakiCtrl *ctrl)
{
	ctrl->crypt_counter_local = 0;
	ctrl->crypt_counter_remote = 0;

	ChiakiSession *session = ctrl->session;
	struct addrinfo *addr = session->connect_info.host_addrinfo_selected;
	struct sockaddr *sa = malloc(addr->ai_addrlen);
	if(!sa)
	{
		CHIAKI_LOGE(session->log, "Ctrl failed to alloc sockaddr");
		return CHIAKI_ERR_MEMORY;
	}
	memcpy(sa, addr->ai_addr, addr->ai_addrlen);

	if(sa->sa_family == AF_INET)
		((struct sockaddr_in *)sa)->sin_port = htons(SESSION_CTRL_PORT);
	else if(sa->sa_family == AF_INET6)
		((struct sockaddr_in6 *)sa)->sin6_port = htons(SESSION_CTRL_PORT);
	else
	{
		CHIAKI_LOGE(session->log, "Ctrl got invalid sockaddr");
		return CHIAKI_ERR_INVALID_DATA;
	}

	chiaki_socket_t sock = socket(sa->sa_family, SOCK_STREAM, IPPROTO_TCP);
	if(CHIAKI_SOCKET_IS_INVALID(sock))
	{
		CHIAKI_LOGE(session->log, "Session ctrl socket creation failed.");
		ctrl_failed(ctrl, CHIAKI_QUIT_REASON_CTRL_UNKNOWN);
		return CHIAKI_ERR_NETWORK;
	}

	ChiakiErrorCode err = chiaki_socket_set_nonblock(sock, true);
	if(err != CHIAKI_ERR_SUCCESS)
		CHIAKI_LOGE(session->log, "Failed to set ctrl socket to non-blocking: %s", chiaki_error_string(err));

	chiaki_mutex_unlock(&ctrl->notif_mutex);
	err = chiaki_stop_pipe_connect(&ctrl->notif_pipe, sock, sa, addr->ai_addrlen);
	chiaki_mutex_lock(&ctrl->notif_mutex);
	free(sa);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		if(err == CHIAKI_ERR_CANCELED)
		{
			if(ctrl->should_stop)
				CHIAKI_LOGI(session->log, "Ctrl requested to stop while connecting");
			else
				CHIAKI_LOGE(session->log, "Ctrl notif pipe signaled without should_stop during connect");
			CHIAKI_SOCKET_CLOSE(sock);
		}
		else
		{
			CHIAKI_LOGE(session->log, "Ctrl connect failed: %s", chiaki_error_string(err));
			ChiakiQuitReason quit_reason = err == CHIAKI_ERR_CONNECTION_REFUSED ? CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED : CHIAKI_QUIT_REASON_CTRL_UNKNOWN;
			ctrl_failed(ctrl, quit_reason);
		}
		goto error;
	}

	CHIAKI_LOGI(session->log, "Ctrl connected to %s:%d", session->connect_info.hostname, SESSION_CTRL_PORT);

	uint8_t auth_enc[CHIAKI_RPCRYPT_KEY_SIZE];
	err = chiaki_rpcrypt_encrypt(&session->rpcrypt, ctrl->crypt_counter_local++, (const uint8_t *)session->connect_info.regist_key, auth_enc, CHIAKI_RPCRYPT_KEY_SIZE);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;
	char auth_b64[CHIAKI_RPCRYPT_KEY_SIZE*2];
	err = chiaki_base64_encode(auth_enc, sizeof(auth_enc), auth_b64, sizeof(auth_b64));
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;

	uint8_t did_enc[CHIAKI_RP_DID_SIZE];
	err = chiaki_rpcrypt_encrypt(&session->rpcrypt, ctrl->crypt_counter_local++, session->connect_info.did, did_enc, CHIAKI_RP_DID_SIZE);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;
	char did_b64[CHIAKI_RP_DID_SIZE*2];
	err = chiaki_base64_encode(did_enc, sizeof(did_enc), did_b64, sizeof(did_b64));
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;

	uint8_t ostype_enc[128];
	size_t ostype_len = strlen(SESSION_OSTYPE) + 1;
	if(ostype_len > sizeof(ostype_enc))
		goto error;
	err = chiaki_rpcrypt_encrypt(&session->rpcrypt, ctrl->crypt_counter_local++, (const uint8_t *)SESSION_OSTYPE, ostype_enc, ostype_len);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;
	char ostype_b64[256];
	err = chiaki_base64_encode(ostype_enc, ostype_len, ostype_b64, sizeof(ostype_b64));
	if(err != CHIAKI_ERR_SUCCESS)
		goto error;

	char bitrate_b64[256];
	bool have_bitrate = session->target >= CHIAKI_TARGET_PS4_10;
	if(have_bitrate)
	{
		uint8_t bitrate[4] = { 0 };
		uint8_t bitrate_enc[4] = { 0 };
		err = chiaki_rpcrypt_encrypt(&session->rpcrypt, ctrl->crypt_counter_local++, (const uint8_t *)bitrate, bitrate_enc, 4);
		if(err != CHIAKI_ERR_SUCCESS)
			goto error;

		err = chiaki_base64_encode(bitrate_enc, 4, bitrate_b64, sizeof(bitrate_b64));
		if(err != CHIAKI_ERR_SUCCESS)
			goto error;
	}

	char streaming_type_b64[256];
	bool have_streaming_type = chiaki_target_is_ps5(session->target);
	if(have_streaming_type)
	{
		uint32_t streaming_type;
		switch(session->connect_info.video_profile.codec)
		{
			case CHIAKI_CODEC_H265:
				streaming_type = 2;
				break;
			case CHIAKI_CODEC_H265_HDR:
				streaming_type = 3;
				break;
			default:
				streaming_type = 1;
				break;
		}
		uint8_t streaming_type_buf[4] = {
			streaming_type & 0xff,
			(streaming_type >> 8) & 0xff,
			(streaming_type >> 0x10) & 0xff,
			(streaming_type >> 0x18) & 0xff
		};
		uint8_t streaming_type_enc[4] = { 0 };
		err = chiaki_rpcrypt_encrypt(&session->rpcrypt, ctrl->crypt_counter_local++,
				streaming_type_buf, streaming_type_enc, 4);
		if(err != CHIAKI_ERR_SUCCESS)
			goto error;

		err = chiaki_base64_encode(streaming_type_enc, 4, streaming_type_b64, sizeof(streaming_type_b64));
		if(err != CHIAKI_ERR_SUCCESS)
			goto error;
	}

	static const char request_fmt[] =
			"GET %s HTTP/1.1\r\n"
			"Host: %s:%d\r\n"
			"User-Agent: remoteplay Windows\r\n"
			"Connection: keep-alive\r\n"
			"Content-Length: 0\r\n"
			"RP-Auth: %s\r\n"
			"RP-Version: %s\r\n"
			"RP-Did: %s\r\n"
			"RP-ControllerType: 3\r\n"
			"RP-ClientType: 11\r\n"
			"RP-OSType: %s\r\n"
			"RP-ConPath: 1\r\n"
			"%s%s%s"
			"%s%s%s"
			"\r\n";

	const char *path;
	if(session->target == CHIAKI_TARGET_PS4_8 || session->target == CHIAKI_TARGET_PS4_9)
		path = "/sce/rp/session/ctrl";
	else if(chiaki_target_is_ps5(session->target))
		path = "/sie/ps5/rp/sess/ctrl";
	else
		path = "/sie/ps4/rp/sess/ctrl";
	const char *rp_version = chiaki_rp_version_string(session->target);

	char buf[512];
	int request_len = snprintf(buf, sizeof(buf), request_fmt,
			path, session->connect_info.hostname, SESSION_CTRL_PORT, auth_b64,
			rp_version ? rp_version : "", did_b64, ostype_b64,
			have_bitrate ? "RP-StartBitrate: " : "",
			have_bitrate ? bitrate_b64 : "",
			have_bitrate ? "\r\n" : "",
			have_streaming_type ? "RP-StreamingType: " : "",
			have_streaming_type ? streaming_type_b64 : "",
			have_streaming_type ? "\r\n" : "");
	if(request_len < 0 || request_len >= sizeof(buf))
		goto error;

	CHIAKI_LOGI(session->log, "Sending ctrl request");
	chiaki_log_hexdump(session->log, CHIAKI_LOG_VERBOSE, (const uint8_t *)buf, (size_t)request_len);

	int sent = send(sock, buf, (size_t)request_len, 0);
	if(sent < 0)
	{
		CHIAKI_LOGE(session->log, "Failed to send ctrl request");
		goto error;
	}

	size_t header_size;
	size_t received_size;
	err = chiaki_recv_http_header(sock, buf, sizeof(buf), &header_size, &received_size, &ctrl->notif_pipe, CTRL_EXPECT_TIMEOUT);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		if(err != CHIAKI_ERR_CANCELED)
		{
#ifdef _WIN32
			int errsv = WSAGetLastError;
#else
			int errsv = errno;
#endif
			CHIAKI_LOGE(session->log, "Failed to receive ctrl request response: %s", chiaki_error_string(err));
			if(err == CHIAKI_ERR_NETWORK)
			{
#ifdef _WIN32
				CHIAKI_LOGE(session->log, "Ctrl request response network error: %d", errsv);
#else
				CHIAKI_LOGE(session->log, "Ctrl request response network error: %s", strerror(errsv));
#endif
			}
		}
		else
		{
			CHIAKI_LOGI(session->log, "Ctrl canceled while receiving ctrl request response");
		}
		goto error;
	}

	CHIAKI_LOGI(session->log, "Ctrl received http header as response");
	chiaki_log_hexdump(session->log, CHIAKI_LOG_VERBOSE, (const uint8_t *)buf, header_size);

	ChiakiHttpResponse http_response;
	err = chiaki_http_response_parse(&http_response, buf, header_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(session->log, "Failed to parse ctrl request response");
		goto error;
	}

	CHIAKI_LOGI(session->log, "Ctrl received ctrl request http response");

	CtrlResponse response;
	parse_ctrl_response(&response, &http_response);
	if(!response.success)
	{
		CHIAKI_LOGE(session->log, "Ctrl http response was not successful. HTTP code was %d", http_response.code);
		chiaki_http_response_fini(&http_response);
		err = CHIAKI_ERR_UNKNOWN;
		goto error;
	}
	chiaki_http_response_fini(&http_response);

	if(response.server_type_valid)
	{
		ChiakiErrorCode err2 = chiaki_rpcrypt_decrypt(&session->rpcrypt,
				ctrl->crypt_counter_remote++,
				response.rp_server_type,
				response.rp_server_type,
				sizeof(response.rp_server_type));
		if(err2 != CHIAKI_ERR_SUCCESS)
		{
			CHIAKI_LOGE(session->log, "Ctrl failed to decrypt RP-Server-Type");
			response.server_type_valid = false;
		}
	}

	if(response.server_type_valid)
	{
		uint8_t server_type = response.rp_server_type[0]; // 0 = PS4, 1 = PS4 Pro, 2 = PS5
		CHIAKI_LOGI(session->log, "Ctrl got Server Type: %u", (unsigned int)server_type);
		if(server_type == 0
				&& session->connect_info.video_profile_auto_downgrade
				&& session->connect_info.video_profile.height == 1080)
		{
			// regular PS4 doesn't support >= 1080p
			CHIAKI_LOGI(session->log, "1080p was selected but server would not support it. Downgrading.");
			chiaki_connect_video_profile_preset(
				&session->connect_info.video_profile,
				CHIAKI_VIDEO_RESOLUTION_PRESET_720p,
				session->connect_info.video_profile.max_fps == 60
					? CHIAKI_VIDEO_FPS_PRESET_60
					: CHIAKI_VIDEO_FPS_PRESET_30);
		}
		if((server_type == 0 || server_type == 1)
				&& session->connect_info.video_profile.codec != CHIAKI_CODEC_H264)
		{
			// PS4 doesn't support anything except h264
			CHIAKI_LOGI(session->log, "A codec other than H264 was selected but server would not support it. Downgrading.");
			session->connect_info.video_profile.codec = CHIAKI_CODEC_H264;
		}
	}
	else
		CHIAKI_LOGE(session->log, "No valid Server Type in ctrl response");

	ctrl->sock = sock;

	// if we already got more data than the header, put the rest in the buffer.
	ctrl->recv_buf_size = received_size - header_size;
	if(ctrl->recv_buf_size > 0)
		memcpy(ctrl->recv_buf, buf + header_size, ctrl->recv_buf_size);

	return CHIAKI_ERR_SUCCESS;

error:
	CHIAKI_SOCKET_CLOSE(sock);
	return err;
}
