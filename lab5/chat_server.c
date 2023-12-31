#include "chat.h"
#include "chat_server.h"

#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netdb.h>
#include <fcntl.h>
#define _GNU_SOURCE
#define BUF_SIZE 1024
#define BUF_LIMIT 10240

struct chat_peer {
	/** Client's socket. To read/write messages. */
	int socket;
	char* name;
	/** Output buffer. 
	 * One for receiving messages and one for sending.
	 * We switch between them as necessary.
	*/
	char* output_buf[2];
	int output_buf_size[2];
	int output_buf_capacity[2];
	int sending_out_buf_id;
	int sending_pointer;
	char* partial_buf;
	int partial_buf_capacity;
	int partial_buf_size;
};

struct chat_server {
	/** Listening socket. To accept new clients. */
	int socket;
	/** Array of peers. */
	struct chat_peer** peers;
	int n_peers;
	/** Array of received messages. */
	int n_received_messages;
	struct chat_message** received_messages;

	struct epoll_event epoll_trigger;
	int epoll_fd;
};

struct chat_server *
chat_server_new(void)
{
	struct chat_server *server = calloc(1, sizeof(struct chat_server));
	if(server == NULL)
	{
		abort();
	}
	server->socket = -1;
	server->n_peers = 0;
	server->peers = NULL;
	server->n_received_messages = 0;
	server->received_messages = NULL;
	return server;
}

void
chat_server_delete(struct chat_server *server)
{
	if (server->socket >= 0)
	{
		close(server->socket);
	}
	for(int i = 0; i < server->n_peers; i++)
	{
		close(server->peers[i]->socket);
		if(server->peers[i]->name != NULL)
		{
			free(server->peers[i]->name);
		}
		for(int j = 0; j < 2; j++)
		{
			if(server->peers[i]->output_buf_capacity[j])
			{
				free(server->peers[i]->output_buf[j]);
			}
		}
		free(server->peers[i]);
	}
	if(server->n_peers)
	{
		free(server->peers);
	}
	for(int i = 0; i < server->n_received_messages; i++)
	{
		chat_message_delete(server->received_messages[i]);
	}
	if(server->received_messages != NULL)
	{
		free(server->received_messages);
	}
	if(server->epoll_fd >= 0)
	{
		close(server->epoll_fd);
	}
	free(server);
}

void delete_peer(struct chat_server* server, int fd)
{
	short int shift = 0;
	for(int i = 0; i < server->n_peers; i++)
	{
		if(server->peers[i]->socket == fd)
		{
			for(int j = 0; j < 2; j++)
			{
				if(server->peers[i]->output_buf_capacity[j])
				{
					free(server->peers[i]->output_buf[j]);
				}
			}
			if(server->peers[i]->name != NULL)
			{
				free(server->peers[i]->name);
			}
			if(server->peers[i]->partial_buf != NULL)
			{
				free(server->peers[i]->partial_buf);
			}
			free(server->peers[i]);
			shift = 1;
		}
		if(shift && i + 1 < server->n_peers)
		{
			server->peers[i] = server->peers[i+1];
		}
	}
	if(shift)
	{
		server->n_peers --;
		server->peers = realloc(server->peers, server->n_peers * sizeof(struct chat_peer*));
	}
	return;
}

int accept_new_peer(struct chat_server* server)
{
	while(1)
	{
		int new_client_fd = accept(server->socket, NULL, NULL);
		if (new_client_fd == -1) {
			/* We have processed all incoming connections. */
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
				break;
			}
			else {
				perror ("Error accepting client at server");
				break;
			}
		}
		if(new_client_fd < 0)
		{
			return CHAT_ERR_SYS;
		}
		struct chat_peer* new_peer = calloc(1, sizeof(struct chat_peer));
		new_peer->name = NULL;
		new_peer->socket = new_client_fd;
		new_peer->output_buf_size[0] = new_peer->output_buf_size[1] = 0;
		new_peer->output_buf_capacity[0] = new_peer->output_buf_capacity[1] = 0;
		new_peer->output_buf[0] = new_peer->output_buf[1] = NULL;
		new_peer->sending_pointer = 0;
		new_peer->partial_buf = NULL;
		new_peer->partial_buf_size = 0;
		new_peer->partial_buf_capacity = 0;
		server->peers = realloc(server->peers, (server->n_peers + 1) * sizeof(struct chat_peer*));
		server->peers[server->n_peers] = new_peer;
		server->n_peers += 1;

		struct epoll_event event;
		event.events = EPOLLIN;
		event.data.ptr = new_peer;
		// Make the new connection non blocking
		int old_flags = fcntl(new_client_fd, F_GETFL);
		fcntl(new_client_fd, F_SETFL, old_flags | O_NONBLOCK);
		if(epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, new_client_fd, &event) < 0)
		{
			epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, new_client_fd, NULL);
			close(new_client_fd);
			free(new_peer);
			return CHAT_ERR_SYS;
		}
	}
	return 0;
}

/*
 * 1) Create a server socket (function socket()).
 * 2) Bind the server socket to addr (function bind()).
 * 3) Listen the server socket (function listen()).
 * 4) Create epoll
 */
int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (server->socket != -1)
	{
        return CHAT_ERR_ALREADY_STARTED;
    }
	
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_port = htons(port);
	/* Listen on all IPs of this machine. */
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server->socket  = socket(AF_INET, SOCK_STREAM, 0);

	if (server->socket < 0) {
        perror("Error creating server socket");
        return CHAT_ERR_SYS;
    }
    int socket_options = 1;
    if (setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &socket_options, sizeof(int)) < 0)
    {
        perror("Error setting server socket options");
        close(server->socket);
        return CHAT_ERR_SYS;
    }
	int old_flags = fcntl(server->socket, F_GETFL);
	fcntl(server->socket, F_SETFL, old_flags | O_NONBLOCK);
    if (bind(server->socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Error binding server socket");
		if (errno == EADDRINUSE) {
            return CHAT_ERR_PORT_BUSY;
        }
        return CHAT_ERR_SYS;
    }
	if(listen(server->socket, SOMAXCONN) < 0)
	{
		perror("Error listening on server");
		close(server->socket);
		return CHAT_ERR_SYS;
	}
	
	int epoll_fd = epoll_create(1);
	server->epoll_fd = epoll_fd;
	if (epoll_fd < 0)
	{
		perror("Error creating epoll\n");
		close(server->socket);
		return CHAT_ERR_SYS;
	}
	server->epoll_trigger.events = EPOLLIN;
	server->epoll_trigger.data.ptr = NULL;
	if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server->socket, &server->epoll_trigger) < 0)
    {
		perror("Error adding server socket to epoll");
        close(server->socket);
		close(epoll_fd);
        return CHAT_ERR_SYS;
    }
    return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	if(server->n_received_messages)
	{
		struct chat_message* ret = server->received_messages[0];
		for(int i = 1; i < server->n_received_messages; i++)
		{
			server->received_messages[i-1]
				= server->received_messages[i];
		}
		server->n_received_messages--;
		server->received_messages = 
			realloc(server->received_messages, (server->n_received_messages) * sizeof(struct chat_message*));
		return ret;
	}
	return NULL;
}

int send_message_to_client(struct chat_server* server, int client_socket, struct chat_peer* client)
{
	if(client == NULL)
	{
		return -1;
	}
	if(client->output_buf_size[client->sending_out_buf_id] == 0)
	{
		return 0;
	}
	int pointer = client->sending_pointer;
	int rc = 0;
	int send_counter = 0;
	int id = client->sending_out_buf_id;
	while(pointer < client->output_buf_size[id])
	{
		// limit sending size
		size_t send_size = client->output_buf_size[id] - pointer;
		if(send_size > BUF_LIMIT)
		{
			send_size = BUF_LIMIT;
		}
		if(send_counter++ == 10)
		{
			rc = -1;
			errno = EWOULDBLOCK;
		}
		else{
			rc = 
				send(client_socket, client->output_buf[id] + pointer,
						send_size, 0);
		}
		if(rc < 0)
		{
			if(errno == EWOULDBLOCK || errno == EAGAIN)
			{
				if(pointer == client->output_buf_size[id])
				{
					client->sending_pointer = 0;
					client->output_buf_size[id] = 0;
					client->sending_out_buf_id = (!id);
				}
				else
				{
					client->sending_pointer = pointer;
				}
				return pointer;
			}
			break;
		}
		else 
		if(rc == 0)
		{
			break;
		}
		else
		{
			pointer += rc;
		}
	}
	// After sending the entire buffer, switch to the other.
	client->sending_pointer = 0;
	client->output_buf_size[id] = 0;
	client->sending_out_buf_id = (!id);
	// If the second buffer is empty, no more OUT.
	if(client->output_buf_size[!id] == 0)
	{
		server->epoll_trigger.events = EPOLLIN;
		server->epoll_trigger.data.ptr = client;
		rc = epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, client_socket, &server->epoll_trigger);
	}
	if(rc < 0)
	{
		return CHAT_ERR_SYS;
	}
	return 0;
}

char* concatenate_author_and_message(struct chat_message* msg)
{
	int author_len = strlen(msg->author);
	int data_len = strlen(msg->data);
	int result_capacity = author_len + data_len + 2;
	char* result_buf = calloc(result_capacity+1, sizeof(char));
	int result_buf_size = 0;
	memcpy(result_buf + result_buf_size, msg->author, author_len);
	result_buf_size += author_len;
	result_buf[result_buf_size++] = '\n';
	memcpy(result_buf + result_buf_size, msg->data, data_len);
	result_buf_size += data_len;
	result_buf[result_buf_size++] = '\n';
	result_buf[result_buf_size] = '\0';

	return result_buf;
}

int add_message_to_clients(struct chat_server* server, struct epoll_event* event, struct chat_message* msg)
{
	char* result_buf = concatenate_author_and_message(msg);
	int sz = strlen(result_buf);
	for(int i = 0; i < server->n_peers; i++)
	{
		int client_socket = server->peers[i]->socket;
		struct chat_peer* client = event->data.ptr;
		if(client_socket == client->socket)
		{
			continue;
		}
		client = server->peers[i];
		int buf_id = (!client->sending_out_buf_id);
		int target_size = client->output_buf_size[buf_id] + sz;
		if(client->output_buf_capacity[buf_id] < target_size+1)
		{
			char* new_buf = calloc(target_size*2+1, sizeof(char));
			memcpy(new_buf, client->output_buf[buf_id], client->output_buf_size[buf_id]);
			memcpy(new_buf + client->output_buf_size[buf_id], result_buf, sz);
			new_buf[target_size] = '\0';
			free(client->output_buf[buf_id]);
			client->output_buf[buf_id] = new_buf;
			client->output_buf_size[buf_id] = target_size;
			client->output_buf_capacity[buf_id] = target_size*2;
		}
		else
		{
			memcpy(client->output_buf[buf_id]+client->output_buf_size[buf_id], result_buf, sz);
			client->output_buf_size[buf_id] += sz;
		}
		// signal to socket that it should write
		server->epoll_trigger.events = (EPOLLOUT | EPOLLIN);
		server->epoll_trigger.data.ptr = server->peers[i];
		int rc = epoll_ctl(server->epoll_fd, EPOLL_CTL_MOD, client_socket, &server->epoll_trigger);
		if(rc < 0)
		{
			free(result_buf);
			return CHAT_ERR_SYS;
		}
		// If the other buffer is empty, make this buffer the sending buffer.
		if(client->output_buf_size[client->sending_out_buf_id] == 0)
		{
			client->sending_out_buf_id = !client->sending_out_buf_id;
		}
	}
	free(result_buf);
	return 0;
}

/*
* 1) Read message from client with as many retries as it takes
* 2) store message somewhere for pop_message to access it
*/
int read_messages_from_client(struct chat_server* server, struct epoll_event* event)
{
	struct chat_peer* client = event->data.ptr;
	int client_fd = client->socket;
	int rc = 0;
	int total_received = 0;
	char* input_buf = NULL;
	int input_buf_capacity = 0;
	int input_buf_size = 0;
	if(client->partial_buf != NULL)
	{
		input_buf = client->partial_buf;
		input_buf_size = client->partial_buf_size;
		input_buf_capacity = client->partial_buf_capacity;
		client->partial_buf = NULL;
		client->partial_buf_capacity = 0;
		client->partial_buf_size = 0;
	}
	if(input_buf_capacity == 0)
	{
		input_buf = calloc(BUF_SIZE, sizeof(char));
		input_buf_capacity = BUF_SIZE;
	}
	int recv_count = 0;
	do{
		input_buf_size += rc;
		total_received += rc;
		if(input_buf_size == input_buf_capacity)
		{
			input_buf_capacity *= 2;
			char* new_buf = calloc(input_buf_capacity, sizeof(char));
			memcpy(new_buf, input_buf, input_buf_size);
			free(input_buf);
			input_buf = new_buf;
		}
		int recv_size = input_buf_capacity - input_buf_size;
		if(recv_size > BUF_LIMIT)
		{
			recv_size = BUF_LIMIT;
		}
		if(recv_count++ == 10)
		{
			break;
		}
		else
		{
			rc = recv(client_fd, input_buf + input_buf_size,
						recv_size, MSG_DONTWAIT);
		}
	}while(rc > 0);

	if(total_received)
	{
		int start = 0;
		for(int i = 0; i < input_buf_size; i++)
		{
			if(input_buf[i] == '\n')
			{
				// Only first time client sends a message, get client name.
				if(client->name == NULL)
				{
					client->name = calloc(i - start + 1, sizeof(char));
					client->name = memcpy(client->name, input_buf, i + 1);
					// add terminating character instead of '\n'
					client->name[i] = '\0';
					// remove name from buf to not be added to other clients.
					char* new_buf = calloc(input_buf_capacity - i, sizeof(char));
					memcpy(new_buf, input_buf+i+1, input_buf_size -i-1);
					new_buf[input_buf_size-i-1] = '\0';
					input_buf_capacity -= i;
					input_buf_size -= (i+1);
					start = 0;
					i = -1;
					free(input_buf);
					input_buf = new_buf;
					continue;
				}
				struct chat_message* new_message = calloc(1, sizeof(struct chat_message));
				char* message_content = calloc(i - start + 1, sizeof(char));
				message_content = memcpy(message_content, input_buf+start, (i - start +1));
				// add terminating character instead of '\n'
				message_content[i-start] = '\0';
				new_message->data = message_content;
				new_message->author = strdup(client->name);
				add_message_to_clients(server, event, new_message);
				server->received_messages = realloc(
								server->received_messages, (server->n_received_messages + 1) * sizeof(struct chat_message*));
				server->received_messages[server->n_received_messages] = new_message;
				server->n_received_messages += 1;
				start = i + 1;
			}
		}
		if(start < input_buf_size)
		{
			char* new_buf = calloc(input_buf_capacity - start+1, sizeof(char));
			memcpy(new_buf, input_buf+start, input_buf_size -start);
			new_buf[input_buf_size-start] = '\0';
			input_buf_capacity -= (start);
			input_buf_size -= (start);
			client->partial_buf = new_buf;
			client->partial_buf_capacity = input_buf_capacity;
			client->partial_buf_size = input_buf_size;
		}
	}
	free(input_buf);
	return total_received;
}

/*
 * 1) Wait on epoll/kqueue/poll for update on any socket.
 * 2) Handle the update.
 */
int
chat_server_update(struct chat_server *server, double timeout)
{
	if(server->socket == -1)
	{
		return CHAT_ERR_NOT_STARTED;
	}
	if(timeout < 0)
	{
		timeout = 0;
	}
	int max_events = server->n_peers + 1;
	if(server->n_peers > 1000)
	{
		max_events = 1000;
	}
	struct epoll_event events[max_events];
	int rc = epoll_wait(server->epoll_fd, &events[0], max_events, timeout * 1000);
	if(rc == 0)
	{
		return CHAT_ERR_TIMEOUT;
	}
	else if(rc < 0)
	{
		perror("Error getting events at server update");
		return CHAT_ERR_SYS;
	}
	for(int i = 0; i < rc; i++)
	{
		if(events[i].data.ptr == NULL)
		{
			accept_new_peer(server);
			continue;
		}
		struct chat_peer* client = events[i].data.ptr;
		int client_socket = client->socket;
		if((events[i].events & EPOLLIN))
		{
			if(read_messages_from_client(server, &events[i]) == 0)
			{
				rc = epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client_socket, NULL);
				if(rc < 0)
				{
					return CHAT_ERR_SYS;
				}
				close(client_socket);
				delete_peer(server, client_socket);
			}
		}
		if((events[i].events & EPOLLOUT))
		{
			send_message_to_client(server, client_socket, client);
		}
	}
	return 0;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */

	/*
	 * Server has multiple sockets - own and from connected clients. Hence
	 * you can't return a socket here. But if you are using epoll/kqueue,
	 * then you can return their descriptor. These descriptors can be polled
	 * just like sockets and will return an event when any of their owned
	 * descriptors has any events.
	 *
	 * For example, assume you created an epoll descriptor and added to
	 * there a listen-socket and a few client-sockets. Now if you will call
	 * poll() on the epoll's descriptor, then on return from poll() you can
	 * be sure epoll_wait() can return something useful for some of those
	 * sockets.
	 */
#endif
	(void)server;
	return -1;
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
	(void)server;
	if(server->socket == -1)
	{
		return 0;
	}
	for(int i = 0; i < server->n_peers; i++)
	{
		if(server->peers[i]->output_buf_size[0] || server->peers[i]->output_buf_size[1])
		{
			return (CHAT_EVENT_INPUT | CHAT_EVENT_OUTPUT);
		}
	}
	return CHAT_EVENT_INPUT;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
	/* IMPLEMENT THIS FUNCTION if want +5 points. */
#endif
	(void)server;
	(void)msg;
	(void)msg_size;
	return CHAT_ERR_NOT_IMPLEMENTED;
}
