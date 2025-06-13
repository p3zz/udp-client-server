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

int server_if_has_ip_address(int socket_fd, const char *iface_name) {
    struct ifreq ifr = {0};

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

int server_extract_if_from_msg(const struct msghdr* msg, char* if_name){
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
    
    if(if_indextoname(pktinfo->ipi_ifindex, if_name) == 0) {
        return 1;
    }

    return 0;
}

int main() {
    int sockfd = 0;
    struct sockaddr_in client_addr = {0};
    uint8_t client_buffer[BUFFER_SIZE];
    char client_control[CMSG_SPACE(sizeof(struct in_pktinfo))];
    char client_ip[INET_ADDRSTRLEN];
    char if_name[IFNAMSIZ] = {0}; // Buffer for interface name
    pthread_t tid;
    struct iovec iov = { .iov_base = client_buffer, .iov_len = sizeof(client_buffer) };
    struct msghdr msg = {
        .msg_name = &client_addr,
        .msg_namelen = sizeof(client_addr),
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = client_control,
        .msg_controllen = sizeof(client_control),
    };

    if(server_configure_socket(&sockfd) != 0){
        perror("socket configuration failed");
    }

    if (pthread_create(&tid, NULL, thread_body, &data) != 0) {
        perror("Failed to create thread");
        return 1;
    }

    printf("Listening for UDP messages on port %d...\n", PORT);

    while (1) {
        ssize_t n = recvmsg(sockfd, &msg, 0);
        if (n < 0) {
            perror("recvfrom failed");
            continue;
        }

        if(server_extract_if_from_msg(&msg, if_name) == 0){
            printf("Message coming from interface %s\n", if_name);
        }else{
            printf("Message coming from unknown interface\n");
            exit(1);
        }

        client_buffer[n] = '\0';  // Null-terminate
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);

        printf("Received message from %s:%d: %s\n",
               client_ip, ntohs(client_addr.sin_port), client_buffer);

        int value = 0;
        pthread_mutex_lock(&data.lock);
        value = data.value;
        pthread_mutex_unlock(&data.lock);

        if(server_if_has_ip_address(sockfd, if_name) == 0){
            printf("IP address is configured for interface %s, sending response on UDP unicast\n", if_name);
            // Send response
            if (sendto(sockfd, &value, sizeof(value), 0,
                       (struct sockaddr *)&client_addr, sizeof(client_addr)) < 0) {
                perror("sendto (response) failed");
                exit(1);
            } else {
                printf("Sent response to %s:%d\n",
                       client_ip, ntohs(client_addr.sin_port));
            }
        }
        // TODO send broadcast only to target interface
        else{
            printf("IP address is not configured for interface %s, sending response on UDP broadcast\n", if_name);
            // Enable broadcast        
            // Prepare broadcast address
            struct sockaddr_in broadcast_addr;
            memset(&broadcast_addr, 0, sizeof(broadcast_addr));
            broadcast_addr.sin_family = AF_INET;
            broadcast_addr.sin_port = client_addr.sin_port; // use same port as client
            broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST); // 255.255.255.255
        
            if (sendto(sockfd, &value, sizeof(value), 0,
                       (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
                perror("sendto (broadcast) failed");
            } else {
                printf("Sent broadcast response on port %d\n", ntohs(broadcast_addr.sin_port));
            }
        }
    }

    close(sockfd);
    return 0;
}
