#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <errno.h>
#include <pthread.h>

#define PORT 12345
#define BUFFER_SIZE 1024
#define RESPONSE "Message received"

typedef struct{
    int value;
    pthread_mutex_t lock;
}shared_data;

static shared_data data = { .value = 0, .lock = PTHREAD_MUTEX_INITIALIZER };

void *thread_body(void *arg) {
    shared_data *shared = (shared_data *)arg;

    while (1) {
        pthread_mutex_lock(&shared->lock);
        shared->value++;
        shared->value %= 10;
        pthread_mutex_unlock(&shared->lock);

        sleep(2); // simulate work / wait time
    }

    return NULL;
}

int server_if_has_ip_address(int socket_fd, int if_idx) {
    struct ifreq ifr = {0};
    char iface_name[IFNAMSIZ] = {0};

     // Convert index to name
    if (if_indextoname(if_idx, iface_name) == NULL) {
        perror("if_indextoname");
        return -1; // Error converting index to name
    }

    strncpy(ifr.ifr_name, iface_name, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(socket_fd, SIOCGIFADDR, &ifr) == -1) {
        // Interface probably doesn't have an IP address
        return 1;
    }
    struct sockaddr_in *ipaddr = (struct sockaddr_in *)&ifr.ifr_addr;
    printf("Interface %s has IP: %s\n", iface_name, inet_ntoa(ipaddr->sin_addr));
    return 0; // Has IP
}

void server_configure_socket_address(struct sockaddr_in* server_addr){
    // address family: internet
    server_addr->sin_family = AF_INET;
    // accept packets from any interface
    server_addr->sin_addr.s_addr = INADDR_ANY;
    // configure port to listen to
    server_addr->sin_port = htons(PORT);
}

int server_configure_socket(int* socket_fd){
    int broadcast_enable = 1;
    int pkt_info_enable = 1;
    struct sockaddr_in server_addr = {0};
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0){
        return 1;
    }
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        return 1;
    }
    if (setsockopt(sockfd, IPPROTO_IP, IP_PKTINFO, &pkt_info_enable, sizeof(pkt_info_enable)) < 0) {
        return 1;
    }
    server_configure_socket_address(&server_addr);
    // Bind socket to the address
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return 1;
    }
    *socket_fd = sockfd;
    return 0;
}

int server_extract_if_from_msg(const struct msghdr* msg, int* if_idx){
    struct in_pktinfo *pktinfo = NULL;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR((struct msghdr*)msg, cmsg)) {
        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
            pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
            break;
        }
    }

    if (pktinfo == NULL) {
        return 1;
    }

    *if_idx = pktinfo->ipi_ifindex;

    return 0;
}

int server_send_broadcast_data(int sockfd, in_port_t port, int if_idx, const uint8_t* data, int data_len){
    uint8_t client_control[CMSG_SPACE(sizeof(struct in_pktinfo))] = {0};

    struct sockaddr_in broadcast_addr = {
        .sin_addr = {
            .s_addr = htonl(INADDR_BROADCAST)
        },
        .sin_family = AF_INET,
        .sin_port = port
    };

    // craft data to send
    struct iovec dest_iov = {
        .iov_base = (uint8_t*)data,
        .iov_len = data_len
    };

    struct msghdr dest_msg = {
        .msg_name = &broadcast_addr,
        .msg_namelen = sizeof(broadcast_addr),
        .msg_iov = &dest_iov,
        .msg_iovlen = 1,
        .msg_control = client_control,
        .msg_controllen = sizeof(client_control)
    };

    // Setup control message for specifying interface
    struct cmsghdr *dest_cmsg = CMSG_FIRSTHDR(&dest_msg);
    if(dest_cmsg == NULL){
        return 1;
    }
    dest_cmsg->cmsg_level = IPPROTO_IP;
    dest_cmsg->cmsg_type = IP_PKTINFO;
    dest_cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));

    struct in_pktinfo dest_pktinfo = {0};

    dest_pktinfo.ipi_ifindex = if_idx;
    memcpy(CMSG_DATA(dest_cmsg), &dest_pktinfo, sizeof(dest_pktinfo));

    dest_msg.msg_controllen = sizeof(client_control);

    // Send the packet
    if (sendmsg(sockfd, &dest_msg, 0) < 0) {
        return 1;
    }
    return 0;
}

int server_send_unicast_data(int sockfd, const struct sockaddr_in* client, const uint8_t* data, int data_len){
    if (sendto(sockfd, data, data_len, 0, (struct sockaddr *)client, sizeof(struct sockaddr_in)) < 0) {
        return 1;
    }
    return 0;
}

int server_receive_data(int sockfd, struct sockaddr_in* client, int* if_idx, uint8_t* data, int capacity, int* data_len){
    uint8_t client_control[CMSG_SPACE(sizeof(struct in_pktinfo))] = {0};
    struct iovec iov = { .iov_base = data, .iov_len = capacity };
    struct msghdr msg = {
        .msg_name = client,
        .msg_namelen = sizeof(struct sockaddr_in),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = client_control,
        .msg_controllen = sizeof(client_control),
    };

    ssize_t n = recvmsg(sockfd, &msg, 0);
    if (n < 0) {
        return 1;
    }

    if(server_extract_if_from_msg(&msg, if_idx) != 0){
        return 1;
    }

    *data_len = n;
    return 0;
}

int main() {
    int sockfd = 0;
    struct sockaddr_in client = {0};
    uint8_t client_buffer[BUFFER_SIZE] = {0};
    char client_ip[INET_ADDRSTRLEN] = {0};
    pthread_t tid;
    
    if(server_configure_socket(&sockfd) != 0){
        perror("socket configuration failed");
    }

    if (pthread_create(&tid, NULL, thread_body, &data) != 0) {
        perror("Failed to create thread");
        return 1;
    }

    printf("Listening for UDP messages on port %d...\n", PORT);

    while (1) {
        int data_len = 0;
        int if_idx = 0;
        if(server_receive_data(sockfd, &client, &if_idx, client_buffer, BUFFER_SIZE, &data_len) != 0){
            printf("Malformed data received");
            exit(1);
        };

        client_buffer[data_len] = '\0';  // Null-terminate
        inet_ntop(AF_INET, &(client.sin_addr), client_ip, INET_ADDRSTRLEN);

        printf("Received message from %s:%d: %s\tfamily: %d\n",
               client_ip, ntohs(client.sin_port), client_buffer, client.sin_family);

        int value = 0;
        pthread_mutex_lock(&data.lock);
        value = data.value;
        pthread_mutex_unlock(&data.lock);

        if(server_if_has_ip_address(sockfd, if_idx) == 0){
            printf("IP address is configured for interface %d, sending response on UDP unicast\n", if_idx);
            // Send response
            if(server_send_unicast_data(sockfd, &client, (uint8_t*)&value, sizeof(value)) != 0){
                perror("sendto (response) failed");
                exit(1);
            }
        }
        // TODO send broadcast only to target interface
        else{
            printf("IP address is not configured for interface %d, sending response on UDP broadcast\n", if_idx);
            if(server_send_broadcast_data(sockfd, client.sin_port, if_idx, (uint8_t*)&value, sizeof(value)) != 0){
                perror("cannot send broadcast message");
                exit(1);
            }
        }
    }

    close(sockfd);
    return 0;
}
