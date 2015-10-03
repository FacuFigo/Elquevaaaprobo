/*
 ============================================================================
 Name        : admSwap.c
 Author      : 
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <commons/config.h>
#include <commons/log.h>
#include <commons/process.h>
#include <commons/string.h>
#include <commons/collections/list.h>
#include <commons/collections/queue.h>
#include "../../sockets.h"

#define BACKLOG 5

//Recepcion de info de Memoria
typedef struct {
	int processID;
	int cantidadDePaginas;
} process_t;

//Struct que va dentro de la lista
typedef struct {
	int numeroPagina;
	int disponibilidad;
	int proceso;
} pagina_t;

//Operaciones
typedef enum {
	INICIARPROCESO,
	ENTRADASALIDA,
	INICIOMEMORIA,
	LEERMEMORIA,
	ESCRIBIRMEMORIA,
	FINALIZARPROCESO,
	RAFAGAPROCESO,
	PROCESOBLOQUEADO
} operacion_t;

//typedef enum {NUEVOPROCESO, LEER, ESCRIBIR, FINALIZAR} procedimiento_t;

t_log* archivoLog;
char* ipAdmMemoria;
char* puertoAdmMemoria;
int puertoEscucha;
char* nombreSwap;
int cantidadPaginas;
int listeningSocket;
int tamanioPagina;
unsigned retardoCompactacion; //son segundos sino lo cambio a int
int clienteMemoria;

t_list* listaGestionEspacios;

pthread_mutex_t mutexProcedimiento;
pthread_mutex_t mutexProceso;

FILE* archivoSwap;

//Funciones de configuración
void configurarAdmSwap(char* config);
int configurarSocketServidor();

//Funciones de gestion de espacios de memoria
void admDeEspacios();
int buscarEspacioDisponible(int espacioNecesario);
void asignarEspacio(int paginaInicio, process_t* proceso);
int liberarMemoria(int pid);
void liberarEspacioEnArchivo(int numeroDePagina);
void escribirLeer();
char* leerPagina(int pid, int numeroPagina);
void escribirPagina(int paginaAEscribir, char* contenido);

int main(int argc, char** argv) {

	//Creo el archivo de logs
	archivoLog = log_create("log_SWAP", "SWAP", 1, 0);
	log_info(archivoLog, "Archivo de logs creado.\n");

	configurarAdmSwap(argv[1]);

	//Configuro el servidor
	configurarSocketServidor();

	struct sockaddr_storage direccionCliente;
	unsigned int len = sizeof(direccionCliente);
	clienteMemoria = accept(listeningSocket, (void*) &direccionCliente, &len);
	log_info(archivoLog, "Se conecta el proceso Memoria %i. \n", clienteMemoria);

	//Creo el archivo de Swap
	int tamanioArchivoSwap = cantidadPaginas * tamanioPagina;

	char* comando = string_from_format("sudo dd if=/dev/sda1 of=%s bs=%i count=%i", nombreSwap, tamanioArchivoSwap, tamanioArchivoSwap);

	if(system(comando) == -1)
		log_error(archivoLog, "No se pudo crear el archivo de Swap.\n");
	else
		log_info(archivoLog, "Se creó el archivo de Swap.\n");

	//Abro e inicializo el archivo con "\0"
	archivoSwap = fopen(nombreSwap, "r+");

	while(!feof(archivoSwap)){
		fputc('\0',archivoSwap);
	}

	//Creo la lista para la gestion de los espacios vacios
	listaGestionEspacios = list_create();
	int i;
	for(i = 0; i < cantidadPaginas; i++){
		pagina_t* pagina = malloc(sizeof(pagina_t));
		pagina->numeroPagina = i;
		pagina->disponibilidad = 1;
		pagina->proceso = -1;
		list_add(listaGestionEspacios, pagina);
		free(pagina);
	}

	pthread_mutex_init(&mutexProcedimiento, NULL);
	pthread_mutex_init(&mutexProceso, NULL);

	//Thread que gestiona los espacios en la lista de gestion
	pthread_t admDeEspacio;
	pthread_create(&admDeEspacio, NULL, (void *) admDeEspacios, NULL);

	//Thread que busca las paginas solicitadas por memoria
	//pthread_t escribirLeerPaginas;
	//pthread_create(&escribirLeerPaginas, NULL, (void *) escribirLeer, NULL);

	pthread_join(admDeEspacio, NULL);
	//pthread_join(escribirLeerPaginas, NULL);

	list_destroy(listaGestionEspacios);
	fclose(archivoSwap);

	return 0;
}

void configurarAdmSwap(char* config) {
	t_config* configurarAdmSwap = config_create(config);

	if (config_has_property(configurarAdmSwap, "PUERTO_ESCUCHA"))
		puertoEscucha = config_get_int_value(configurarAdmSwap,	"PUERTO_ESCUCHA");
	if (config_has_property(configurarAdmSwap, "NOMBRE_SWAP"))
		nombreSwap = string_duplicate(config_get_string_value(configurarAdmSwap, "NOMBRE_SWAP"));
	if (config_has_property(configurarAdmSwap, "CANTIDAD_PAGINAS"))
		cantidadPaginas = config_get_int_value(configurarAdmSwap, "CANTIDAD_PAGINAS");
	if (config_has_property(configurarAdmSwap, "TAMANIO_PAGINA"))
		tamanioPagina = config_get_int_value(configurarAdmSwap,	"TAMANIO_PAGINA");
	if (config_has_property(configurarAdmSwap, "RETARDO_COMPACTACION"))
		retardoCompactacion = config_get_int_value(configurarAdmSwap, "RETARDO_COMPACTACION");

	config_destroy(configurarAdmSwap);
}

int configurarSocketServidor() {

	struct sockaddr_in direccionServidor;
	direccionServidor.sin_family = AF_INET;
	direccionServidor.sin_addr.s_addr = INADDR_ANY;
	direccionServidor.sin_port = htons(puertoEscucha);

	listeningSocket = socket(AF_INET, SOCK_STREAM, 0);

	int activado = 1;
	setsockopt(listeningSocket, SOL_SOCKET, SO_REUSEADDR, &activado,
			sizeof(activado));

	if (bind(listeningSocket, (void*) &direccionServidor,
			sizeof(direccionServidor)) != 0) {
		log_error(archivoLog, "Falló el bind");
		return 1;
	}

	listen(listeningSocket, BACKLOG);

	log_info(archivoLog, "Servidor creado. %i\n", listeningSocket);

	return 1;
}

void admDeEspacios(){

	process_t* proceso = malloc(sizeof(process_t));
	int paginaInicio = 0;
	int operacion, tamanioPaquete;

	log_info(archivoLog, "Comienza el hilo Administrador de Espacios.\n");

	while(1){

		recibirYDeserializarInt(&operacion, clienteMemoria);

		switch(operacion){
			case INICIARPROCESO:{

				//Recibir proceso de Memoria - PID, CANTIDAD DE PAGINAS A OCUPAR -
				recibirYDeserializarInt(&proceso->processID, clienteMemoria);
				recibirYDeserializarInt(&proceso->cantidadDePaginas, clienteMemoria);

				paginaInicio = buscarEspacioDisponible(proceso->cantidadDePaginas);

				tamanioPaquete = sizeof(int);
				char* paquete = malloc(tamanioPaquete);

				if(paginaInicio == -1){
					//No hay espacio disponible
					log_info(archivoLog, "No hay paginas disponibles para el proceso %i.\n", proceso->processID);

					serializarInt(paquete, -1);
					send(clienteMemoria, paquete, tamanioPaquete, 0);

					free(paquete);
				} else{
					//Se guarda en la lista de gestion de procesos el proceso que acaba de entrar a memoria
					asignarEspacio(paginaInicio, proceso);
					log_info(archivoLog, "Se le asignaron las paginas al proceso %i.\n", proceso->processID);

					serializarInt(paquete, 1);
					send(clienteMemoria, paquete, tamanioPaquete, 0);

					free(paquete);
				}
				break;
			}

			case FINALIZARPROCESO:{

				//Recibe la peticion por parte de Memoria de eliminar un proceso
				recibirYDeserializarInt(&proceso->processID, clienteMemoria);

				tamanioPaquete = sizeof(int);
				char* paquete = malloc(tamanioPaquete);

				if(liberarMemoria(proceso->processID) == -1){
					log_error(archivoLog, "No se pudo finalizar el proceso %i.\n", &proceso->processID);

					serializarInt(paquete, -1);
					send(clienteMemoria, paquete, tamanioPaquete, 0);

					free(paquete);
				}else{
					//Enviar confirmacion
					serializarInt(paquete, 1);
					send(clienteMemoria, paquete, tamanioPaquete, 0);

					free(paquete);
				}
				break;
			}

			case LEERMEMORIA:{

				int* pid = malloc(sizeof(int));
				recibirYDeserializarInt(pid, clienteMemoria);

				int* paginaALeer = malloc(sizeof(int));
				recibirYDeserializarInt(paginaALeer, clienteMemoria);

				char* contenidoPagina = malloc(tamanioPagina);
				contenidoPagina = leerPagina(*pid, *paginaALeer);

				//Enviar contenidoPagina a Memoria
				tamanioPaquete = tamanioPagina + sizeof(int) * 2;
				char* paquete = malloc(tamanioPaquete);

				serializarChar(serializarInt(paquete, operacion), contenidoPagina);

				send(clienteMemoria, paquete, tamanioPaquete, 0);

				free(paginaALeer);
				free(contenidoPagina);
				free(paquete);

				break;
			}

			case ESCRIBIRMEMORIA:{

				int* paginaAEscribir = malloc(sizeof(int));
				recibirYDeserializarInt(paginaAEscribir, clienteMemoria);

				int* tamanioContenido = malloc(sizeof(int));
				recibirYDeserializarInt(tamanioContenido, clienteMemoria);

				char* contenido = malloc(*tamanioContenido);
				recibirYDeserializarChar(&contenido, clienteMemoria);

				escribirPagina(*paginaAEscribir, contenido);

				free(paginaAEscribir);
				//Enviar confirmacion y contenido a Memoria
				tamanioPaquete = *tamanioContenido + sizeof(int) * 2;

				char* paquete = malloc(tamanioPaquete);

				serializarChar(serializarInt(paquete, operacion), contenido);

				send(clienteMemoria, paquete, tamanioPaquete, 0);

				free(paquete);
				free(tamanioContenido);
				free(contenido);

				break;
			}
		}
	}

	free(proceso);
}

//Si encontro el espacio, devuelve el byte en donde comienza el espacio, sino devuelve -1
int buscarEspacioDisponible(int paginasNecesarias){

	int paginaInicio = 0;
	int paginasEncontradas = 0;
	int numeroPagina = 0;
	int encontrado = 0;
	pagina_t* pagina = malloc(sizeof(pagina_t));

	pagina = list_get(listaGestionEspacios, numeroPagina);
	paginaInicio = pagina->numeroPagina;

	while(numeroPagina <= cantidadPaginas){

		if(pagina->disponibilidad == 1){
			paginasEncontradas++;
		}else{
			paginasEncontradas = 0;
			paginaInicio = numeroPagina + 1;
		}

		if(paginasEncontradas == paginasNecesarias){
			encontrado++;
			break;
		}else{
			numeroPagina++;
			pagina = list_get(listaGestionEspacios, numeroPagina);
		}
	}

	if(!encontrado)
		return -1;

	free(pagina);
	return paginaInicio;
}

void asignarEspacio(int paginaInicio, process_t* proceso){

	int i;
	int paginaALeer;
	pagina_t* pagina = malloc(sizeof(pagina_t));

	pagina = list_get(listaGestionEspacios, paginaInicio);
	paginaALeer = pagina->numeroPagina;

	for(i = 0; i < proceso->cantidadDePaginas; i++){
		//Asigno No Disponible y el ID del Proceso que la ocupa
		pagina->disponibilidad = 0;
		pagina->proceso = proceso->processID;

		paginaALeer++;
		pagina = list_get(listaGestionEspacios, paginaInicio);

	}

	free(pagina);
}

//Libera la parte de memoria ocupada por el proceso
int liberarMemoria(int pid){
	int numero = 0;

	pagina_t* pagina = malloc(sizeof(pagina_t));
	pagina = list_get(listaGestionEspacios, numero);

	while(pagina->proceso != pid){
		numero++;
		pagina = list_get(listaGestionEspacios, numero);
	}

	while(pagina->proceso == pid){

		liberarEspacioEnArchivo(pagina->numeroPagina);

		pagina->disponibilidad = 1;
		pagina->proceso = -1;
		numero++;
		pagina = list_get(listaGestionEspacios, numero);
	}

	return 0;
}

void liberarEspacioEnArchivo(int numeroDePagina){
	int numeroByte;
	int posicion = numeroDePagina * tamanioPagina;

	fseek(archivoSwap, posicion,SEEK_SET);

	for(numeroByte = 0; numeroByte <= tamanioPagina; numeroByte++){
		fputc('\0', archivoSwap);
	}

}

//TODO Thread para realizar la escritura y lectura de memoria - CHECKPOINT 3 -
void escribirLeer(){

}

char* leerPagina(int pid, int numeroPagina){

	pagina_t* pagina = malloc(sizeof(pagina_t));
	int numero = 0;
	int paginaProceso = 0;

	char* contenido = malloc(tamanioPagina);

	while(pagina->proceso != pid){
		numero++;
		pagina = list_get(listaGestionEspacios, numero);
	}

	while(pagina->proceso == pid){

		if(paginaProceso == numeroPagina){

			int posicion = pagina->numeroPagina * tamanioPagina;

			fseek(archivoSwap, posicion, SEEK_SET);

			fgets(contenido, tamanioPagina, archivoSwap);

			string_trim(&contenido);

			break;
		}else{
			paginaProceso++;
			numero++;
			pagina = list_get(listaGestionEspacios, numero);
		}

	}
	return contenido;
}

//TODO Cambiar por algo parecido a leerPagina
void escribirPagina(int paginaAEscribir, char* contenido){

	int posicion = paginaAEscribir * tamanioPagina;

	fseek(archivoSwap, posicion, SEEK_SET);

	fputs(contenido, archivoSwap);

}
