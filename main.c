#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <pthread.h>

#include "SerialManager.h"

enum lineas {
    A,
    B,
    C,
    D,

    lines_qty
};

#define SET                         1
#define CLEAR                       0
#define TRUE                        1
#define FALSE                       0

#define OPEN_PORT                   1
#define BAUD_RATE                   115200

#define BUFFER_TO_CIAA              ">OUTS:%d,%d,%d,%d\r\n"
#define SIZE_TO_CIAA                sizeof(BUFFER_TO_CIAA) + 1
#define BUFFER_FROM_CIAA            ">TOGGLE STATE:%d\r\n"
#define SIZE_FROM_CIAA              sizeof(BUFFER_FROM_CIAA) + 1
#define SIZE_ACCEPT_CIAA_DATA       (SIZE_FROM_CIAA - 3)
#define DATA_FROM_CIAA_INDEX        14
#define BUFFER_TO_INTERFACE         ":LINE%dTG\n"
#define SIZE_TO_INTERFACE           sizeof(BUFFER_TO_INTERFACE) + 1
#define BUFFER_FROM_INTERFACE       ":STATES%d%d%d%d\n"
#define DATA_FROM_INTERF_INDEX      7
#define SIZE_FROM_INTERFACE         12//sizeof(BUFFER_FROM_INTERFACE) + 1
#define ASCII_TO_INT(x)              (x - '0')
#define INT_TO_ASCII(x)              (x + '0')

#define SERVER_BACKLOG              (2 * lines_qty)

#define SIGACTON_ERROR              -1
#define CONECTION_ERROR             -1
#define ERROR                       -1

//======================== DECLARACIÓN DE VARIABLES GLOBALES ===========================
int8_t line[lines_qty] = {0, 0, 0, 0};

char dataToCIAA[SIZE_TO_CIAA];
char dataToInterface[SIZE_TO_INTERFACE];
int8_t flagInterfaceToCIAA = CLEAR;
int8_t flagCIAAToInterface = CLEAR;
int sock_fd;
int newfd;
int socketState = 0;
volatile sig_atomic_t kill_process = FALSE;
pthread_t interface_thread;

static pthread_mutex_t mutexZone = PTHREAD_MUTEX_INITIALIZER;

//========================= DECLARACIÓN DE FUNCIONES PRIVADAS ==========================

/**
 * @brief función que setea el flag kill_process para terminar el programa
 *
 * @param sig parametro que pasa la señal recibida
 */
void recibiSignal(int sig); // prototipo de funcion de callback de sigaction


/**
 * @brief Bloquea señales para lanzar el thread con señales bloqueadas y herede esta configuración
 *
 */
void bloquearSign(void);

/**
 * @brief Desbloquea señales luego de crear el Thread
 *
 */
void desbloquearSign(void);

/**
 * @brief Función de Thread que gestiona la señal desde Interface a CIAA
 *
 */
void*interfaceRoutine(void *threadInterfaceMsg);


//================= Rutina de hilo principal ===========================================
int main(void) {
    struct sigaction sa_1, sa_2;

    const char *interfaceMsg;

    // Se carga handler 1 para SIGUSR1
    sa_1.sa_handler = recibiSignal;
    sa_1.sa_flags = 0; //SA_1_RESTART;
    sigemptyset(&sa_1.sa_mask);

    if (sigaction(SIGINT, &sa_1, NULL) == SIGACTON_ERROR) {
        perror("sigaction\n\r");
        exit(EXIT_FAILURE);
    }

    // Se carga handler 2 para SIGUSR2
    sa_2.sa_handler = recibiSignal;
    sa_2.sa_flags = 0; //SA_2_RESTART;
    sigemptyset(&sa_2.sa_mask);

    if (sigaction(SIGTERM, &sa_2, NULL) == SIGACTON_ERROR) {
        perror("sigaction\n\r");
        exit(EXIT_FAILURE);
    }

    //==============================

    struct sockaddr_in serveraddr;

    // Creamos socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Cargamos datos de IP:PORT del server
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(10000);
    serveraddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (serveraddr.sin_addr.s_addr == INADDR_NONE) {
        fprintf(stderr, "ERROR invalid server IP\r\n");
        exit(1);
    }

    // Abrimos puerto con bind()
    if (bind(sock_fd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) == CONECTION_ERROR) {
        close(sock_fd);
        perror("listener: bind");
        exit(1);
    }

    // Seteamos socket en modo Listening
    if (listen(sock_fd, SERVER_BACKLOG) == CONECTION_ERROR) {
        perror("error en listen");
        exit(1);
    }

    //==============================

    bloquearSign();

    //======================== Inicio de threads ======================
    int retI = pthread_create(&interface_thread,
                              NULL,
                              interfaceRoutine,
                              &interfaceMsg);
    if (0 != retI) {
        errno = retI;
        perror("pthreadI_create");
        return ERROR;
    }

    desbloquearSign();

    //======================= Comienza segmento de dato de CIAA a Interface =====================

    printf("Inicio Serial Service\r\n");

    int line_toggled = 0;

    char dataFromCIAA[SIZE_FROM_CIAA];          //dato recibido de EDU-CIAA ">TOGGLE STATE:%d\r\n"
    memset(dataFromCIAA, 0, SIZE_FROM_CIAA);      //inicializo el arreglo en 0

    pthread_mutex_lock(&mutexZone);
    snprintf(dataToCIAA, SIZE_TO_CIAA, BUFFER_TO_CIAA, line[A], line[B], line[C], line[D]); // creo el mensaje de inicialización para la placa. Arranco en 0.
    pthread_mutex_unlock(&mutexZone);


    if (!serial_open(OPEN_PORT, BAUD_RATE)) {
        serial_send(dataToCIAA, SIZE_TO_CIAA); //debería tener cargado el valor inicial de las lineas
        printf("se inicializa la placa \n\r");
        sleep(1);
    }

    memset(dataFromCIAA, 0, SIZE_FROM_CIAA); // Borro los datos del mensaje.

    while (1) {
        // Leemos información desde EDU-CIAA
        if (serial_receive(dataFromCIAA, SIZE_FROM_CIAA) > 0) {
            if (strlen(dataFromCIAA) >= (SIZE_ACCEPT_CIAA_DATA)) {
                line_toggled = ASCII_TO_INT(dataFromCIAA[DATA_FROM_CIAA_INDEX]);
                flagCIAAToInterface = SET;
                memset(dataFromCIAA, 0, SIZE_FROM_CIAA);
            }
        }

        pthread_mutex_lock(&mutexZone);
        if ((SET == flagCIAAToInterface) && (line_toggled <= lines_qty) && (socketState == SET)) {
            flagCIAAToInterface = CLEAR;

            snprintf(dataToInterface, SIZE_TO_INTERFACE, BUFFER_TO_INTERFACE, line_toggled);

            // Enviamos mensaje a cliente
            if (write(newfd, dataToInterface, SIZE_TO_INTERFACE) == ERROR) {
                perror("Error escribiendo mensaje en socket");
                exit(1);
            }
        }
        pthread_mutex_unlock(&mutexZone);

        if (TRUE == kill_process) {
            void *thread_cancel;
            pthread_mutex_lock(&mutexZone);
            pthread_cancel(interface_thread);
            pthread_join(interface_thread, &thread_cancel);
            if (thread_cancel == PTHREAD_CANCELED) {
                printf("%s", "Thread was canceled\r\n");
            }
            else {
                printf("%s", "Thread ended\r\n");
            }
            serial_close();
            close(sock_fd);
            close(newfd);
            pthread_mutex_unlock(&mutexZone);
            exit(EXIT_SUCCESS);
            break;
        }

        usleep(20000);
    }

    exit(EXIT_SUCCESS);
    return 0;
}

//=========== Thread de comunicación Interface ========================

void*interfaceRoutine(void *interfaceMsg) {
    printf("Inicio Interface Routine\r\n");

    struct sockaddr_in clientaddr;
    socklen_t addr_len;

    char dataFromInterface[SIZE_FROM_INTERFACE];

    int n;

    while (1) {
        // Ejecutamos accept() para recibir conexiones entrantes
        printf("Esperando Accept\r\n");
        addr_len = sizeof(struct sockaddr_in);
        if ((newfd = accept(sock_fd, (struct sockaddr *)&clientaddr, &addr_len)) == CONECTION_ERROR) {
            perror("error en accept");
            exit(1);
        }
        printf("server:  conexion desde:  %s\n", inet_ntoa(clientaddr.sin_addr));
        socketState = SET;
        while (1) {
            // Leemos mensaje de cliente
            if ((n = read(newfd, dataFromInterface, SIZE_FROM_INTERFACE)) == ERROR || n <= 0) {
                socketState = CLEAR;
                printf("SocketCleared. \r\n");
                break;
            }

            if (n == SIZE_FROM_INTERFACE) {
                n = 0;
                pthread_mutex_lock(&mutexZone);
                line[A] = ASCII_TO_INT(dataFromInterface[DATA_FROM_INTERF_INDEX + A]);
                line[B] = ASCII_TO_INT(dataFromInterface[DATA_FROM_INTERF_INDEX + B]);
                line[C] = ASCII_TO_INT(dataFromInterface[DATA_FROM_INTERF_INDEX + C]);
                line[D] = ASCII_TO_INT(dataFromInterface[DATA_FROM_INTERF_INDEX + D]);

                if (!serial_open(OPEN_PORT, BAUD_RATE)) {
                    snprintf(dataToCIAA, SIZE_TO_CIAA, BUFFER_TO_CIAA, line[A], line[B], line[C], line[D]);
                    printf("dataToCIAA: %s\r\n", dataToCIAA);
                    serial_send(dataToCIAA, SIZE_TO_CIAA);
                }
                pthread_mutex_unlock(&mutexZone);
            }

            usleep(20000);
        }
    }
}

void recibiSignal(int sig) {
    if (SIGINT == sig || SIGTERM == sig) {
        printf("\r\n Signal exit received \n\r");
        kill_process = TRUE;
    }
}

void bloquearSign(void) {
    sigset_t set;
    sigemptyset(&set);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

void desbloquearSign(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}
