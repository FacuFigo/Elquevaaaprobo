/*
 ============================================================================
 Name        : admMemoria.c
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
#include <arpa/inet.h>
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
#include <commons/collections/dictionary.h>
#include <sockets.h>

#define BACKLOG 10

t_log* archivoLog;
t_log* archivoLogPrueba;
t_log* archivoLogObligatorio;

typedef enum {
	INICIARPROCESO = 0,
	ENTRADASALIDA = 1,
	INICIOMEMORIA = 2,
	LEERMEMORIA = 3,
	ESCRIBIRMEMORIA = 4,
	FINALIZARPROCESO = 5,
	RAFAGAPROCESO = 6,
	PROCESOBLOQUEADO = 7
} operacion_t;

typedef enum{
	FIFO=0,
	LRU=1,
	CLOCKMEJORADO=2
} algoritmo_t;

typedef struct {
	int nroPagina;
	int pid;
	int nroMarco;
	int bitPresencia;
	int bitUso;
	int bitModificado;
	int tiempoLRU;
	int tiempoFIFO;
} pagina_t;

typedef struct {
	int pid;
	int pagina;
	int marco;
} entradaTLB_t;

typedef struct {
	int accesosAPaginas;
	int fallosDePaginas;
} accesos_t;

int puertoEscucha;
char* ipSwap;
int puertoSwap;
int maximoMarcosPorProceso;
int cantidadMarcos;
int listeningSocket;
int tamanioMarco;
int entradasTLB;
char* TLBHabilitada;
int retardoMemoria;
int socketSwap;
int clienteCPU;
void* memoriaPrincipal;
int* marcos;
int aciertosTLB=0;
int accesosTLB=0;
algoritmo_t algoritmoDeReemplazo = FIFO;
t_dictionary *tablaDeProcesos;
t_dictionary *tablaDeProcesosCM;
t_dictionary *tablaDeProcesosAccesos;
t_list *tlb;
pthread_mutex_t mutexAccesoTLB;
pthread_mutex_t mutexAccesoMemoria;

typedef enum{iniciar, leer, escribir, entradaSalida, finalizar} t_instruccion;

void configurarAdmMemoria(char* config);
int configurarSocketCliente(char* ip, int puerto, int*);
int configurarSocketServidor();
void admDeMemoria();
void iniciarProceso(int pid, int cantPaginas);
int finalizarProceso(int pid);
int leerMemoria(int pid, int pagina, void* contenido);
int escribirMemoria(int pid,int pagina, void* contenido);
int asignarNuevoMarco();
int cantidadMarcosAsignados(t_dictionary *tablaDePaginas);
void actualizarTiempoLRU(char* key, pagina_t* value);
void actualizarTiempoFIFO(char* key, pagina_t* value);
void tablaDePaginasDestroy(t_dictionary *tablaDePaginas);
void paginaDestroy(pagina_t *pagina);
void desasignarMarcos(char *key, pagina_t* value);
int escribirEnSwap(char *contenido,int pid, int pagina);
int leerDeSwap(int pid, int pagina,char* contenido);
int paginaAReemplazarPorAlgoritmo(t_dictionary *tablaDePaginas);
int tlbHabilitada();
int buscarEnTLBYEscribir(int pid,int pagina,char* contenido);
int buscarEnTLBYLeer(int pid,int pagina,char* contenido);
int eliminarEntradaEnTLB(int pid, int pagina);
void eliminarProcesoEnTLB(char* key, pagina_t* value);
void agregarEntradaEnTLB(int pid, int pagina, int marco);
void signalHandler (int signal);
void tlbFlush();
void vaciarTLB(entradaTLB_t *value);
void memoryFlush();
void limpiarProceso(char* key, t_dictionary *value);
void limpiarProcesoCM(char* key, t_list *value);
void limpiarPaginaYActualizarSwap (char* key, pagina_t *value);
void calculoTasaTLB();
int hayMarcoLibre();

int main(int argc, char** argv) {
	//Creo el archivo de logs
	system("rm log_prueba");

	archivoLog = log_create("log_AdmMemoria", "AdmMemoria", 0, 0);
	archivoLogPrueba = log_create("log_prueba", "AdmMemoria",0,0);
	archivoLogObligatorio = log_create("log_AdmMemoria_Obligatorio", "AdmMemoria",0,0);
	log_info(archivoLog, "Archivo de logs creado.\n");

	configurarAdmMemoria(argv[1]);

	if (configurarSocketCliente(ipSwap, puertoSwap,	&socketSwap))
		log_info(archivoLog, "Conectado al Administrador de Swap %i.\n", socketSwap);
	else
		log_error(archivoLog, "Error al conectar en el Administrador de Swap. %s %i \n", ipSwap, puertoSwap);

	configurarSocketServidor();

	struct sockaddr_storage direccionCliente;
	unsigned int len = sizeof(direccionCliente);
	clienteCPU = accept(listeningSocket, (void*) &direccionCliente, &len);
	log_info(archivoLog, "Se conecta el proceso CPU %d\n", clienteCPU);

	char* paquete = malloc(sizeof(int));
	serializarInt(paquete, tamanioMarco);
	send(clienteCPU, paquete, sizeof(int), 0);
	free(paquete);

	pthread_mutex_init(&mutexAccesoTLB,NULL);
	pthread_mutex_init(&mutexAccesoMemoria,NULL);

	if (signal(SIGUSR1, signalHandler) == SIG_ERR)
	{
		log_info(archivoLog,"Error while installing a signal handler.\n");
		exit(EXIT_FAILURE);
	}
	if (signal(SIGUSR2, signalHandler) == SIG_ERR)
		{
			log_info(archivoLog,"Error while installing a signal handler.\n");
			exit(EXIT_FAILURE);
		}
	if (signal(SIGPOLL, signalHandler) == SIG_ERR)
		{
			log_info(archivoLog,"Error while installing a signal handler.\n");
			exit(EXIT_FAILURE);
		}

	//admDeMemoria();
	pthread_t hiloMemoria;
	pthread_t hiloTasaTLB;
	pthread_create(&hiloMemoria, NULL, (void *)admDeMemoria, NULL);
	pthread_create(&hiloTasaTLB, NULL, (void *)calculoTasaTLB, NULL);

	pthread_join(hiloMemoria, NULL);

	return 0;
}

void configurarAdmMemoria(char* config) {

	t_config* configurarAdmMemoria = config_create(config);
	if (config_has_property(configurarAdmMemoria, "PUERTO_ESCUCHA"))
		puertoEscucha = config_get_int_value(configurarAdmMemoria, "PUERTO_ESCUCHA");
	if (config_has_property(configurarAdmMemoria, "IP_SWAP"))
		ipSwap = string_duplicate(config_get_string_value(configurarAdmMemoria, "IP_SWAP"));
	if (config_has_property(configurarAdmMemoria, "PUERTO_SWAP"))
		puertoSwap = config_get_int_value(configurarAdmMemoria, "PUERTO_SWAP");
	if (config_has_property(configurarAdmMemoria, "MAXIMO_MARCOS_POR_PROCESO"))
		maximoMarcosPorProceso = config_get_int_value(configurarAdmMemoria, "MAXIMO_MARCOS_POR_PROCESO");
	if (config_has_property(configurarAdmMemoria, "CANTIDAD_MARCOS"))
		cantidadMarcos = config_get_int_value(configurarAdmMemoria,	"CANTIDAD_MARCOS");
	if (config_has_property(configurarAdmMemoria, "TAMANIO_MARCOS"))
		tamanioMarco = config_get_int_value(configurarAdmMemoria, "TAMANIO_MARCOS");
	if (config_has_property(configurarAdmMemoria, "ENTRADAS_TLB"))
		entradasTLB = config_get_int_value(configurarAdmMemoria, "ENTRADAS_TLB");
	if (config_has_property(configurarAdmMemoria, "TLB_HABILITADA"))
		TLBHabilitada = string_duplicate(config_get_string_value(configurarAdmMemoria, "TLB_HABILITADA"));
	if (config_has_property(configurarAdmMemoria, "RETARDO_MEMORIA"))
		retardoMemoria = config_get_int_value(configurarAdmMemoria, "RETARDO_MEMORIA");
	if (config_has_property(configurarAdmMemoria, "ALGORITMO_REEMPLAZO")){
		if(string_equals_ignore_case(config_get_string_value(configurarAdmMemoria, "ALGORITMO_REEMPLAZO"),"FIFO"))
				algoritmoDeReemplazo = FIFO;
		if(string_equals_ignore_case(config_get_string_value(configurarAdmMemoria, "ALGORITMO_REEMPLAZO"),"LRU"))
				algoritmoDeReemplazo = LRU;
		if(string_equals_ignore_case(config_get_string_value(configurarAdmMemoria, "ALGORITMO_REEMPLAZO"),"CLOCK_MODIFICADO"))
				algoritmoDeReemplazo = CLOCKMEJORADO;
	}
	config_destroy(configurarAdmMemoria);
}

int configurarSocketCliente(char* ip, int puerto, int* s) {
	struct sockaddr_in direccionServidor;
	direccionServidor.sin_family = AF_INET;
	direccionServidor.sin_addr.s_addr = inet_addr(ip);
	direccionServidor.sin_port = htons(puerto);

	*s = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(*s, (void*) &direccionServidor, sizeof(direccionServidor)) == -1) {
		log_error(archivoLog, "No se pudo conectar");
		return 0;
	}

	return 1;
}

int configurarSocketServidor() {
	struct sockaddr_in direccionServidor;
	direccionServidor.sin_family = AF_INET;
	direccionServidor.sin_addr.s_addr = INADDR_ANY;
	direccionServidor.sin_port = htons(puertoEscucha);

	listeningSocket = socket(AF_INET, SOCK_STREAM, 0);

	int activado = 1;
	setsockopt(listeningSocket, SOL_SOCKET, SO_REUSEADDR, &activado, sizeof(activado));

	if (bind(listeningSocket, (void*) &direccionServidor, sizeof(direccionServidor)) != 0) {
		log_error(archivoLog, "Falló el bind");
		return 1;
	}

	listen(listeningSocket, BACKLOG);


	log_info(archivoLog, "Servidor creado. %i\n", listeningSocket);

	return 1;
}

void admDeMemoria(){
	int i,continuar=1;
	memoriaPrincipal = calloc (cantidadMarcos,tamanioMarco);
	tablaDeProcesos = dictionary_create();
	tablaDeProcesosAccesos = dictionary_create();
	if (algoritmoDeReemplazo==CLOCKMEJORADO)
		tablaDeProcesosCM=dictionary_create();
	if (tlbHabilitada())
		tlb = list_create();
	marcos = malloc(sizeof(int)*cantidadMarcos);
	for (i=0; i<cantidadMarcos;i++)//inicializo todos los marcos a 0 (vacios)
		marcos[i]=0;
	while(continuar){
		int instruccion;
		continuar=recibirYDeserializarInt(&instruccion, clienteCPU);
		if (continuar){
			switch(instruccion){
			case INICIOMEMORIA:{

				int pid, cantPaginas, tamanioPaquete, verificador;
				char *paquete;

				recibirYDeserializarInt(&pid, clienteCPU);
				recibirYDeserializarInt(&cantPaginas, clienteCPU);
				log_info(archivoLog, "Recibi pid %i.\n", pid);
				log_info(archivoLog, "Recibi cantidad de paginas %i.\n", cantPaginas);
				pthread_mutex_lock(&mutexAccesoMemoria);
				iniciarProceso(pid, cantPaginas); //TODO hacer un if para comprobar que lo hizo correctamente
				//Le pido a Swap que inicialice un proceso:
				tamanioPaquete = sizeof(int) * 3;
				paquete = malloc(tamanioPaquete);
				serializarInt(serializarInt(serializarInt(paquete, INICIARPROCESO), pid), cantPaginas);

				send(socketSwap, paquete, tamanioPaquete, 0);
				free(paquete);//Se puede sacar este free?

				//Recibo respuesta de Swap:
				recibirYDeserializarInt(&verificador, socketSwap);
				pthread_mutex_unlock(&mutexAccesoMemoria);

				if (verificador != -1){
					log_info(archivoLogObligatorio,"Proceso mProc creado, pid: %i con %i paginas asignadas.",pid,cantPaginas);
					log_info(archivoLog, "Memoria inicializada");
				}
				else{
					log_info(archivoLogObligatorio,"Fallo al iniciar proceso mProc, pid %i .",pid);
					log_info(archivoLog,"Fallo al inicializar memoria");
				}

				paquete = malloc(sizeof(int));//realloc?
				//Le contesto a CPU
				serializarInt(paquete,verificador);
				send(clienteCPU, paquete, sizeof(int),0);

				free(paquete);
				log_info(archivoLog, "Termino inicializar");
				break;
			}
			case LEERMEMORIA:{
				int pid, pagina, tamanioPaquete, verificador, tlbHit=0;
				char *paquete;

				void* contenido = calloc(1,tamanioMarco);
				if(contenido == NULL)
					log_info(archivoLog,"Error en el malloc");

				log_info(archivoLog, "Empieza leer memoria.");
				recibirYDeserializarInt(&pid, clienteCPU);
				log_info(archivoLog, "Recibí pid: %i",pid);
				recibirYDeserializarInt(&pagina, clienteCPU);
				log_info(archivoLog, "Recibí página: %i",pagina);

				log_info(archivoLogObligatorio,"Solicitud de leer memoria recibida. Pid: %i, página: %i",pid,pagina);

				if (tlbHabilitada()){
					tlbHit = buscarEnTLBYLeer(pid,pagina,contenido);
				}

				if(!tlbHit){//tlb miss o tlb deshabilitada
					pthread_mutex_lock(&mutexAccesoMemoria);
					verificador=leerMemoria(pid,pagina,contenido);
					pthread_mutex_unlock(&mutexAccesoMemoria);
				}
				if (verificador != -1 || tlbHit){
					log_info(archivoLog, "Página %d leida: %s",pagina,contenido);
					tamanioPaquete = sizeof(int)*2+strlen(contenido)+1;
					paquete = malloc(tamanioPaquete);//realloc?
					serializarChar(serializarInt(paquete, verificador),contenido);
					free(contenido);
				}
				else{
					log_info(archivoLog,"Fallo al leer página %d.", pagina);
					tamanioPaquete = sizeof(int);
					paquete = malloc(sizeof(int));
					serializarInt(paquete,verificador);
					free(contenido);
				}
				//Le contesto a CPU
				send(clienteCPU,paquete,tamanioPaquete,0);

				free(paquete);

				break;
			}
			case ESCRIBIRMEMORIA:{
				int pid, pagina, tamanioPaquete, verificador, tlbHit=0;
				char *paquete, *contenido;
				log_info(archivoLog, "Empieza escribir memoria.");
				recibirYDeserializarInt(&pid, clienteCPU);
				log_info(archivoLog, "Recibí pid: %i",pid);
				recibirYDeserializarInt(&pagina, clienteCPU);
				log_info(archivoLog, "Recibí página: %i",pagina);
				recibirYDeserializarChar(&contenido,clienteCPU);
				log_info(archivoLog, "Recibí contenido: %s",contenido);

				log_info(archivoLogObligatorio,"Solicitud de escribir memoria recibida. Pid: %i, página: %i",pid,pagina);

				if (tlbHabilitada()){
					tlbHit = buscarEnTLBYEscribir(pid,pagina,contenido);
				}

				if(!tlbHit){//tlb miss o tlb deshabilitada
					pthread_mutex_lock(&mutexAccesoMemoria);
					verificador=escribirMemoria(pid,pagina,contenido);
					pthread_mutex_unlock(&mutexAccesoMemoria);
				}
				if (verificador != -1 || tlbHit){
					log_info(archivoLog, "Página %d escrita: %s",pagina,contenido);
					tamanioPaquete = sizeof(int)*2+strlen(contenido)+1;
					paquete = malloc(tamanioPaquete);//realloc?
					serializarChar(serializarInt(paquete, verificador),contenido);
					free(contenido);
				}else{
					log_info(archivoLog,"Fallo al escribir página %d. verificador: %i", pagina,verificador);
					tamanioPaquete = sizeof(int);
					paquete = malloc(sizeof(int));
					serializarInt(paquete,verificador);
					free(contenido);
				}
				//Le contesto a CPU
				send(clienteCPU,paquete,tamanioPaquete,0);

				free(paquete);
				break;
			}
			case FINALIZARPROCESO:{
				int pid, tamanioPaquete, verificador;
				char *paquete;

				recibirYDeserializarInt(&pid, clienteCPU);

				pthread_mutex_lock(&mutexAccesoMemoria);
				if(finalizarProceso(pid))
					log_info(archivoLog, "Finalizó correctamente en administrador de memoria");
				else
					log_info(archivoLog, "Error al finalizar en administrador de memoria");

				tamanioPaquete = sizeof(int) * 2;
				paquete = malloc(tamanioPaquete);

				//Le pido a Swap que finalice un proceso:
				serializarInt(serializarInt(paquete, FINALIZARPROCESO), pid);

				send(socketSwap, paquete, tamanioPaquete, 0);

				free(paquete);//Se puede sacar este free?

				//Recibo la respuesta de Swap:
				recibirYDeserializarInt(&verificador, socketSwap);
				pthread_mutex_unlock(&mutexAccesoMemoria);

				if (verificador != -1)
					log_info(archivoLog, "Finalizó correctamente en Swap.");
				else
					log_info(archivoLog, "Fallo al finalzar en Swap.");

				paquete = malloc(sizeof(int));//realloc?
				//Le contesto a CPU
				serializarInt(paquete,verificador);
				send(clienteCPU, paquete, sizeof(int),0);

				free(paquete);

				break;
			}
			}
		}

	}
	log_info(archivoLog,"Terminó hilo de memoria inesperadamente");
}

void iniciarProceso(int pid, int cantPaginas){
	t_dictionary* nuevaTablaDePaginas = dictionary_create();
	int nroPagina;
	if(algoritmoDeReemplazo==CLOCKMEJORADO){
		t_list *punteroClockMejorado=list_create();
		dictionary_put(tablaDeProcesosCM,string_itoa(pid),punteroClockMejorado);
	}
	for (nroPagina=0;nroPagina < cantPaginas;nroPagina++){
		pagina_t* nuevoProceso = malloc(sizeof(pagina_t));
		nuevoProceso->nroPagina=nroPagina;
		nuevoProceso->pid = pid;
		nuevoProceso->nroMarco = -1;
		nuevoProceso->bitPresencia = 0;
		nuevoProceso->bitUso = 0;
		nuevoProceso->bitModificado = 0;
		nuevoProceso->tiempoLRU = 0;
		nuevoProceso->tiempoFIFO=0;
		dictionary_put(nuevaTablaDePaginas,string_itoa(nroPagina),nuevoProceso);
	}
	dictionary_put(tablaDeProcesos,string_itoa(pid),nuevaTablaDePaginas);

	accesos_t* nuevoAcceso = malloc(sizeof(accesos_t));
	nuevoAcceso->accesosAPaginas=0;
	nuevoAcceso->fallosDePaginas=0;
	dictionary_put(tablaDeProcesosAccesos,string_itoa(pid),nuevoAcceso);
}

int finalizarProceso(int pid){
	t_dictionary* tablaDePaginas = dictionary_get(tablaDeProcesos,string_itoa(pid));
	dictionary_iterator(tablaDePaginas,(void*)desasignarMarcos);
	if (tlbHabilitada())
		dictionary_iterator(tablaDePaginas,(void*)eliminarProcesoEnTLB);
	if (algoritmoDeReemplazo==CLOCKMEJORADO){
		t_list* punteroClockMejorado = dictionary_remove(tablaDeProcesosCM,string_itoa(pid));
		list_destroy(punteroClockMejorado);
	}
	accesos_t *accesos = dictionary_remove(tablaDeProcesosAccesos,string_itoa(pid));//TODO imprimir
	log_info(archivoLogObligatorio,"Proceso mProc finalizado con %i PF, y %i accesos a paginas.",accesos->fallosDePaginas,accesos->accesosAPaginas);
	free(accesos);
	dictionary_remove_and_destroy(tablaDeProcesos,string_itoa(pid),(void*)tablaDePaginasDestroy);
	return 1;//o -1 en error
}

int leerMemoria(int pid, int pagina, void*contenido){
	int success;
	t_dictionary *tablaDePaginas = dictionary_get(tablaDeProcesos,string_itoa(pid));
	pagina_t *paginaALeer = dictionary_get(tablaDePaginas, string_itoa(pagina));
	accesos_t *accesos=dictionary_get(tablaDeProcesosAccesos, string_itoa(pid));
	accesos->accesosAPaginas++;
	usleep(retardoMemoria);
	if (paginaALeer->bitPresencia==0){//fallo de pagina
		log_info(archivoLogObligatorio,"Fallo de página, pid: %i",pid);
		if(cantidadMarcosAsignados(tablaDePaginas)<maximoMarcosPorProceso && hayMarcoLibre()){//asigna un nuevo marco
					paginaALeer->nroMarco = asignarNuevoMarco();
					if(algoritmoDeReemplazo==CLOCKMEJORADO){
						t_list *punteroClockMejorado = dictionary_get(tablaDeProcesosCM,string_itoa(pid));
						list_add(punteroClockMejorado,paginaALeer);
					}
					log_info(archivoLogPrueba,"PF de pagina: %i",pagina);
		}else{
			if (cantidadMarcosAsignados(tablaDePaginas)==0){
				log_info(archivoLog,"Error al asignar el primer marco, el proceso se finalizará incorrectamente.");
				finalizarProceso(pid);
				return -1;
			}
			//reemplaza un marco
			log_info(archivoLogPrueba,"PF de pagina: %i",pagina);
			int paginaAReemplazar= paginaAReemplazarPorAlgoritmo(tablaDePaginas);
			log_info(archivoLogPrueba,"pagina a reemplazar: %i",paginaAReemplazar);
			log_info(archivoLogObligatorio,"Encontrada victima para reemplazo, pagina: %i",paginaAReemplazar);
			pagina_t *victima = dictionary_get(tablaDePaginas,string_itoa(paginaAReemplazar));
			paginaALeer->nroMarco=victima->nroMarco;
			victima->bitPresencia=0;
			victima->bitUso=0;
			victima->tiempoLRU=0;
			victima->tiempoFIFO=0;
			if(tlbHabilitada()){
				if (!eliminarEntradaEnTLB(pid,paginaAReemplazar))
					log_info(archivoLog,"No habia entradas en la TLB para pid: %i, pagina: %i, marco: %i",pid,paginaAReemplazar,victima->nroMarco);
			}
			if(victima->bitModificado==1){
				void* aux = malloc(tamanioMarco);
				memcpy(aux,memoriaPrincipal+paginaALeer->nroMarco*tamanioMarco,tamanioMarco);
				usleep(retardoMemoria);
				success=escribirEnSwap(aux,pid,paginaAReemplazar);
				victima->bitModificado=0;
				free(aux);
			}
			if(algoritmoDeReemplazo==CLOCKMEJORADO){
				t_list *punteroClockMejorado = dictionary_get(tablaDeProcesosCM,string_itoa(pid));
				list_add(punteroClockMejorado,paginaALeer);
			}
		}
		accesosTLB++;//para emular el ciclo de leer memoria
		aciertosTLB++;
		success = leerDeSwap(pid,pagina,contenido);
		log_info(archivoLog,"Contenido de swap es: %s",contenido);
		memcpy(memoriaPrincipal+paginaALeer->nroMarco*tamanioMarco,contenido,tamanioMarco);
		accesos->fallosDePaginas++;

		usleep(retardoMemoria);

	}else{
		memcpy(contenido,memoriaPrincipal+paginaALeer->nroMarco*tamanioMarco,tamanioMarco);
		usleep(retardoMemoria);
	}
	log_info(archivoLogObligatorio,"Acceso a memoria realizado para pid: %i, página: %i, marco: %i",pid,pagina,paginaALeer->nroMarco);

	//paginaALeer->bitModificado=0;
	paginaALeer->bitPresencia=1;
	paginaALeer->tiempoLRU=1;
	paginaALeer->bitUso=1;
	dictionary_iterator(tablaDePaginas,(void*)actualizarTiempoLRU);
	dictionary_iterator(tablaDePaginas,(void*)actualizarTiempoFIFO);
	//dictionary_put(tablaDePaginas,string_itoa(pagina),paginaALeer);
	//dictionary_put(tablaDeProcesos,string_itoa(pid),tablaDePaginas);
	if(tlbHabilitada())
		agregarEntradaEnTLB(pid, pagina, paginaALeer->nroMarco);
	return success;
}


int escribirMemoria(int pid, int pagina, void* contenido){
	int success=1;
	void *marcoAEscribir = calloc(1,tamanioMarco);
	t_dictionary *tablaDePaginas = dictionary_get(tablaDeProcesos,string_itoa(pid));
	pagina_t *paginaAEscribir = dictionary_get(tablaDePaginas, string_itoa(pagina));
	accesos_t *accesos=dictionary_get(tablaDeProcesosAccesos, string_itoa(pid));
	accesos->accesosAPaginas++;
	usleep(retardoMemoria);
	log_info(archivoLog,"Terminó de obtener la pagina y el marco es: %i\n",paginaAEscribir->nroMarco);
	if (paginaAEscribir->bitPresencia==0){//fallo de pagina
		log_info(archivoLogObligatorio,"Fallo de página, pid: %i",pid);
		if(cantidadMarcosAsignados(tablaDePaginas)<maximoMarcosPorProceso && hayMarcoLibre()){//asigna un nuevo marco
			paginaAEscribir->nroMarco = asignarNuevoMarco();
			log_info(archivoLog,"Terminó de asignar marco nuevo: %i\n",paginaAEscribir->nroMarco);
			if(algoritmoDeReemplazo==CLOCKMEJORADO){
				t_list *punteroClockMejorado = dictionary_get(tablaDeProcesosCM,string_itoa(pid));
				list_add(punteroClockMejorado,paginaAEscribir);
			}
			log_info(archivoLogPrueba,"PF de pagina: %i",pagina);
		}else{//reemplaza un marco
			if (cantidadMarcosAsignados(tablaDePaginas)==0){
				log_info(archivoLog,"Error al asignar el primer marco, el proceso se finalizará incorrectamente.");
				finalizarProceso(pid);
				return -1;
			}
			log_info(archivoLogPrueba,"PF de pagina: %i",pagina);
			log_info(archivoLog,"Empieza el algoritmo de reemplazo\n");
			int paginaAReemplazar = paginaAReemplazarPorAlgoritmo(tablaDePaginas);
			log_info(archivoLogPrueba,"pagina a reemplazar: %i",paginaAReemplazar);
			log_info(archivoLog,"Encontrada victima para reemplazo, pagina: %i",paginaAReemplazar);
			log_info(archivoLogObligatorio,"Encontrada victima para reemplazo, pagina: %i",paginaAReemplazar);
			pagina_t *victima = dictionary_get(tablaDePaginas,string_itoa(paginaAReemplazar));
			usleep(retardoMemoria);
			paginaAEscribir->nroMarco=victima->nroMarco;
			victima->bitPresencia=0;
			victima->bitUso=0;
			victima->tiempoLRU=0;
			victima->tiempoFIFO=0;
			if (tlbHabilitada()){
				if (!eliminarEntradaEnTLB(pid,paginaAReemplazar))
					log_info(archivoLog,"No habia entradas en la TLB para pid: %i, pagina: %i, marco: %i",pid,paginaAReemplazar,victima->nroMarco);
			}
			if(victima->bitModificado==1){
				log_info(archivoLog,"El bit modificado es 1 y mando a escribir a swap");
				void* aux = malloc(tamanioMarco);
				memcpy(aux,memoriaPrincipal+paginaAEscribir->nroMarco*tamanioMarco,tamanioMarco);
				usleep(retardoMemoria);
				success=escribirEnSwap(aux,pid,paginaAReemplazar);
				victima->bitModificado=0;
				free(aux);
			}
			if(algoritmoDeReemplazo==CLOCKMEJORADO){
				t_list *punteroClockMejorado = dictionary_get(tablaDeProcesosCM,string_itoa(pid));
				list_add(punteroClockMejorado,paginaAEscribir);
			}
		}
		accesosTLB++;//para emular el ciclo de escribir memoria
		aciertosTLB++;
		void* contenidoDeSwap = malloc(tamanioMarco);
		success = leerDeSwap(pid,pagina,contenidoDeSwap);
		log_info(archivoLog,"Contenido de swap es: %s",contenidoDeSwap);
		memcpy(memoriaPrincipal+paginaAEscribir->nroMarco*tamanioMarco,contenidoDeSwap,tamanioMarco);

		accesos->fallosDePaginas++;
		usleep(retardoMemoria);//escribir en memoria
		free(contenidoDeSwap);
	}

	memcpy(marcoAEscribir,contenido,strlen(contenido));//tamanioContenido
	memcpy(memoriaPrincipal+paginaAEscribir->nroMarco*tamanioMarco,marcoAEscribir,tamanioMarco);

	log_info(archivoLogObligatorio,"Acceso a memoria realizado para pid: %i, página: %i, marco: %i",pid,pagina,paginaAEscribir->nroMarco);

	usleep(retardoMemoria);

	paginaAEscribir->bitModificado=1;
	paginaAEscribir->bitPresencia=1;
	paginaAEscribir->bitUso=1;
	paginaAEscribir->tiempoLRU=0;
	//paginaAEscribir->tiempoFIFO=1;
	dictionary_iterator(tablaDePaginas,(void*)actualizarTiempoLRU);
	dictionary_iterator(tablaDePaginas,(void*)actualizarTiempoFIFO);
	//dictionary_put(tablaDePaginas,string_itoa(pagina),process); ahora es innecesario
	//dictionary_put(tablaDeProcesos,string_itoa(pid),tablaDePaginas);
	if(tlbHabilitada())
		agregarEntradaEnTLB(pid, pagina, paginaAEscribir->nroMarco);
	free(marcoAEscribir);
	return success;// debería devolver -1 en error
}

int asignarNuevoMarco(){//first fit
	int nuevoMarco=-1;
	int encontrado =0;
	int i=0;
	while(i<cantidadMarcos&&!encontrado){
		if (marcos[i]==0){
			marcos[i]=1;
			nuevoMarco = i;
			encontrado =1;
		}
		i++;
	}
	return nuevoMarco;
}

int cantidadMarcosAsignados(t_dictionary *tablaDePaginas){
	int cantidad=0;
	int i=0;
	while(i<dictionary_size(tablaDePaginas) && cantidad<maximoMarcosPorProceso){

		pagina_t *aux = dictionary_get(tablaDePaginas,string_itoa(i));
		if(aux->bitPresencia==1)
			cantidad++;
		i++;
	}
	return cantidad;
}

void actualizarTiempoLRU(char* key, pagina_t* value) {
   if(value->bitPresencia==1)
	value->tiempoLRU++;
}

void actualizarTiempoFIFO(char* key, pagina_t* value) {
   if(value->bitPresencia==1)
	value->tiempoFIFO++;
}

void desasignarMarcos(char* key, pagina_t* value){
	if(value->bitPresencia==1){
		marcos[value->nroMarco]=0;
	}
}

void tablaDePaginasDestroy(t_dictionary *tablaDePaginas){
	dictionary_destroy_and_destroy_elements(tablaDePaginas,(void*)paginaDestroy);
}

void paginaDestroy(pagina_t* pagina){
	free(pagina);
}

int escribirEnSwap(char *contenido,int pid, int pagina){
	log_info(archivoLog,"Entre a escribirEnSwap");
	int tamanioPaquete, verificador;
	char *paquete;
	tamanioPaquete = sizeof(int) * 4+strlen(contenido)+1;
	paquete = malloc(tamanioPaquete);

	serializarChar(serializarInt(serializarInt(serializarInt(paquete, ESCRIBIRMEMORIA), pid), pagina),contenido);

	send(socketSwap, paquete, tamanioPaquete, 0);

	free(paquete);//está de más este free?

	recibirYDeserializarInt(&verificador, socketSwap);
	return verificador;

}

int leerDeSwap(int pid, int pagina,char* contenido){
	int tamanioPaquete, verificador;
	char *paquete;
	tamanioPaquete = sizeof(int) * 3;
	paquete = malloc(tamanioPaquete);

	serializarInt(serializarInt(serializarInt(paquete, LEERMEMORIA), pid), pagina);

	send(socketSwap, paquete, tamanioPaquete, 0);

	free(paquete);//está de más este free?

	recibirYDeserializarInt(&verificador, socketSwap);
	if (verificador != -1){
			int status;
			int buffer_size;
			char *buffer = malloc(buffer_size = sizeof(uint32_t));
			uint32_t message_long;
			status = recv(socketSwap, buffer, buffer_size, 0);
			memcpy(&message_long, buffer, buffer_size);
			log_info(archivoLog, "El mesaje_long que manda Swap es: %i y el status es: %i",message_long,status);
			status = recv(socketSwap, contenido, message_long, 0);
			free(buffer);
			log_info(archivoLog, "Página %i leida de Swap: %s y el status es: %i",pagina,contenido, status);
	}else
		log_info(archivoLog, "Error al traer página: %i de Swap",pagina);
	return verificador;
}

int paginaAReemplazarPorAlgoritmo(t_dictionary *tablaDePaginas){
	int i=0;
	int paginaAReemplazar=-1;
	int tiempoMaximo=-1;
	t_list *punteroClockMejorado;
	if(algoritmoDeReemplazo==CLOCKMEJORADO){
		pagina_t* aux= dictionary_get(tablaDePaginas,string_itoa(i));
		punteroClockMejorado = dictionary_get(tablaDeProcesosCM,string_itoa(aux->pid));
		pagina_t *a = list_get(punteroClockMejorado,0);
		log_info(archivoLogPrueba,"El puntero apunta a la pagina: %i con U: %i y M: %i",a->nroPagina,a->bitUso,a->bitModificado);
		log_info(archivoLogObligatorio,"El puntero al empezar el algoritmo de reemplazo apunta a la pagina: %i con U: %i y M: %i",a->nroPagina,a->bitUso,a->bitModificado);
	}
	switch(algoritmoDeReemplazo){
	case FIFO:
		log_info(archivoLogObligatorio,"Imprimiendo páginas en memoria para algoritmo FIFO.");
		while(i<dictionary_size(tablaDePaginas)){
			pagina_t* aux= dictionary_get(tablaDePaginas,string_itoa(i));
			if(aux->bitPresencia==1){
				log_info(archivoLogObligatorio,"Página: %i tiempo FIFO de: %i .",aux->tiempoFIFO);
				if(aux->tiempoFIFO>tiempoMaximo){
					tiempoMaximo=aux->tiempoFIFO;
					paginaAReemplazar=i;
				}
			}
			i++;
		}
		break;
	case LRU:
		log_info(archivoLogObligatorio,"Imprimiendo páginas en memoria para algoritmo LRU.");
		while(i<dictionary_size(tablaDePaginas)){
			pagina_t* aux= dictionary_get(tablaDePaginas,string_itoa(i));
			if(aux->bitPresencia==1){
				log_info(archivoLogObligatorio,"Página: %i tiempo LRU de: %i .",aux->tiempoLRU);
				if(aux->tiempoLRU>tiempoMaximo){
					tiempoMaximo=aux->tiempoLRU;
					paginaAReemplazar=i;
				}
			}
			i++;
		}
		break;
	case CLOCKMEJORADO:
		while (paginaAReemplazar==-1){
			i=0;
			while(i<list_size(punteroClockMejorado)&&paginaAReemplazar==-1){
				pagina_t* aux = list_remove(punteroClockMejorado,0);
				if (aux->bitModificado==0&&aux->bitUso==0){
					paginaAReemplazar=aux->nroPagina;
				}else
					list_add(punteroClockMejorado,aux);
				i++;
			}
			i=0;
			while(i<list_size(punteroClockMejorado)&&paginaAReemplazar==-1){
				pagina_t* aux = list_remove(punteroClockMejorado,0);
				if (aux->bitModificado==1&&aux->bitUso==0){
					paginaAReemplazar=aux->nroPagina;
				}else{
					aux->bitUso=0;
					list_add(punteroClockMejorado,aux);
				}
				i++;
			}
		}
		pagina_t *a = list_get(punteroClockMejorado,0);
		log_info(archivoLogObligatorio,"El puntero al finalizar el algoritmo de reemplazo apunta a la pagina: %i con U: %i y M: %i",a->nroPagina,a->bitUso,a->bitModificado);

		break;
	}
	return paginaAReemplazar;
}

int tlbHabilitada(){
	if(string_equals_ignore_case(TLBHabilitada,"si"))
		return 1;
	else
		return 0;
}

int buscarEnTLBYEscribir(int pid,int pagina,char* contenido){
	int tlbHit =0, i =0, marcoTLB=0;
	accesosTLB++;
	pthread_mutex_lock(&mutexAccesoTLB);
	while(i<list_size(tlb)&&!tlbHit){
		entradaTLB_t *aux = list_get(tlb,i);
		if (aux->pid==pid && aux->pagina==pagina){
			void *paginaAEscribir = calloc(1,tamanioMarco);
			marcoTLB = aux->marco;
			tlbHit =1;
			aciertosTLB++;
			log_info(archivoLog,"TLB hit para escritura de pid: %i, pagina: %i, marco: %i",pid,pagina,marcoTLB);
			log_info(archivoLogObligatorio,"TLB hit para escritura de pid: %i, pagina: %i, marco: %i",pid,pagina,marcoTLB);
			memcpy(paginaAEscribir,contenido,strlen(contenido));//tamanioContenido
			memcpy(memoriaPrincipal+marcoTLB*tamanioMarco,paginaAEscribir,tamanioMarco);
			t_dictionary* tablaAux = dictionary_get(tablaDeProcesos,string_itoa(pid));
			pagina_t *paginaAux = dictionary_get(tablaAux,string_itoa(pagina));
			accesos_t *accesos=dictionary_get(tablaDeProcesosAccesos, string_itoa(pid));
			accesos->accesosAPaginas++;
			paginaAux->bitModificado=1;
			paginaAux->bitUso=1;
			paginaAux->tiempoLRU=0;
			dictionary_iterator(tablaAux,(void*)actualizarTiempoLRU);
			dictionary_iterator(tablaAux,(void*)actualizarTiempoFIFO);
			free(paginaAEscribir);
			usleep(retardoMemoria);
		}
		i++;
	}
	pthread_mutex_unlock(&mutexAccesoTLB);
	return tlbHit;
}

int buscarEnTLBYLeer(int pid,int pagina,char* contenido){
	int tlbHit =0, i =0, marcoTLB=0;
	accesosTLB++;
	pthread_mutex_lock(&mutexAccesoTLB);
	while(i<list_size(tlb)&&!tlbHit){
		entradaTLB_t *aux = list_get(tlb,i);
		if (aux->pid==pid && aux->pagina==pagina){
			marcoTLB = aux->marco;
			tlbHit =1;
			aciertosTLB++;
			log_info(archivoLog,"TLB hit para lectura de pid: %i, pagina: %i, marco: %i",pid,pagina,marcoTLB);
			log_info(archivoLogObligatorio,"TLB hit para lectura de pid: %i, pagina: %i, marco: %i",pid,pagina,marcoTLB);
			memcpy(contenido,memoriaPrincipal+marcoTLB*tamanioMarco,tamanioMarco);
			t_dictionary* tablaAux = dictionary_get(tablaDeProcesos,string_itoa(pid));
			pagina_t *paginaAux = dictionary_get(tablaAux,string_itoa(pagina));
			accesos_t *accesos=dictionary_get(tablaDeProcesosAccesos, string_itoa(pid));
			accesos->accesosAPaginas++;
			paginaAux->bitUso=1;
			paginaAux->tiempoLRU=0;
			//paginaAEscribir->tiempoFIFO=1;
			dictionary_iterator(tablaAux,(void*)actualizarTiempoLRU);
			dictionary_iterator(tablaAux,(void*)actualizarTiempoFIFO);
			usleep(retardoMemoria);
		}
		i++;
	}
	pthread_mutex_unlock(&mutexAccesoTLB);
	return tlbHit;
}

int eliminarEntradaEnTLB(int pid, int pagina){
	int tlbHit =0, i =0, marcoTLB=0, pos=-1;
	pthread_mutex_lock(&mutexAccesoTLB);
	while(i<list_size(tlb)&&!tlbHit){
			entradaTLB_t *aux = list_get(tlb,i);
			if (aux->pid==pid && aux->pagina==pagina){
				marcoTLB = aux->marco;
				tlbHit =1;
				pos = i;
			}
			i++;
		}
	if (tlbHit){
		entradaTLB_t *aux = list_remove(tlb,pos);
		log_info(archivoLog,"Eliminada entrada en TLB para pid: %i, pagina: %i, marco: %i",pid,pagina,marcoTLB);
		log_info(archivoLogObligatorio,"Eliminada entrada en TLB para pid: %i, pagina: %i, marco: %i",pid,pagina,marcoTLB);
		free(aux);
	}
	pthread_mutex_unlock(&mutexAccesoTLB);
	return tlbHit;
}

void eliminarProcesoEnTLB(char* key, pagina_t* value) {
   if(value->bitPresencia==1)
	 eliminarEntradaEnTLB(value->pid,strtol(key,NULL,10));
}

void agregarEntradaEnTLB(int pid, int pagina, int marco){
	entradaTLB_t *nuevaEntrada = malloc(sizeof(entradaTLB_t));
	nuevaEntrada->marco=marco;
	nuevaEntrada->pid=pid;
	nuevaEntrada->pagina=pagina;

	if(list_size(tlb)==entradasTLB){
		pthread_mutex_lock(&mutexAccesoTLB);
		entradaTLB_t *aux = list_remove(tlb,0);
		pthread_mutex_unlock(&mutexAccesoTLB);
		log_info(archivoLog,"Reemplazo una entrada en la TLB");
		log_info(archivoLog,"Elimino la entrada pid: %i, página: %i, marco: %i",aux->pid,aux->pagina,aux->marco);
		log_info(archivoLogObligatorio,"Reemplazo una entrada en la TLB");
		log_info(archivoLogObligatorio,"Elimino la entrada pid: %i, página: %i, marco: %i",aux->pid,aux->pagina,aux->marco);
		free(aux);
	}
	log_info(archivoLog,"Agrego una nueva entrada en la TLB pid: %i, página: %i, marco: %i",nuevaEntrada->pid, nuevaEntrada->pagina, nuevaEntrada->marco);
	log_info(archivoLogObligatorio,"Agrego una nueva entrada en la TLB pid: %i, página: %i, marco: %i",nuevaEntrada->pid, nuevaEntrada->pagina, nuevaEntrada->marco);
	pthread_mutex_lock(&mutexAccesoTLB);
	list_add(tlb,nuevaEntrada);
	pthread_mutex_unlock(&mutexAccesoTLB);
}

void signalHandler (int signal){
	switch(signal){
	case SIGUSR1:
		log_info(archivoLogObligatorio,"Señal SIGUSR1 recibida. Empieza TLB flush.");
		log_info(archivoLog,"TLB flush");
		pthread_t hiloTLBFlush;
		pthread_create(&hiloTLBFlush,NULL,(void*)tlbFlush,NULL);
		log_info(archivoLogObligatorio,"Tratamiento de señal SIGUSR1 terminado.");
		break;
	case SIGUSR2:
		log_info(archivoLogObligatorio,"Señal SIGUSR2 recibida. Empieza Memory flush. Se mandarán a escribir, de haber, las páginas modificadas a Swap.");
		log_info(archivoLog,"Memory flush");
		memoryFlush();
		log_info(archivoLogObligatorio,"Tratamiento de señal SIGUSR2 terminado.");
		break;
	case SIGPOLL:
		log_info(archivoLogObligatorio,"Señal SIGPOLL recibida. Empieza Memory dump.");
		log_info(archivoLog,"Memory copy");
		if (fork()==0){
			int i;
			void *contenido=malloc(tamanioMarco);
			for (i=0;i<cantidadMarcos;i++){
				if(marcos[i]==1){
					memset(contenido,0,tamanioMarco);
					memcpy(contenido,memoriaPrincipal+i*tamanioMarco,tamanioMarco);
					log_info(archivoLogPrueba,"Marco %i, contenido: %s .",i,contenido);
					log_info(archivoLogObligatorio,"Marco %i, contenido: %s .",i, contenido);
				}
			}
			log_info(archivoLogObligatorio,"Tratamiento de señal SIGPOLL terminado.");
			exit(0);
		}

		break;
	}
}

void tlbFlush(){
	pthread_mutex_lock(&mutexAccesoTLB);
	if (tlbHabilitada())
		list_clean_and_destroy_elements(tlb,(void*)vaciarTLB);
	pthread_mutex_unlock(&mutexAccesoTLB);
}

void vaciarTLB(entradaTLB_t *value){
	free(value);
}

void memoryFlush(){
	tlbFlush();
	pthread_mutex_lock(&mutexAccesoMemoria);
	if(!dictionary_is_empty(tablaDeProcesos))
		dictionary_iterator(tablaDeProcesos,(void*)limpiarProceso);
	if(algoritmoDeReemplazo==CLOCKMEJORADO&&!dictionary_is_empty(tablaDeProcesosCM))
		dictionary_iterator(tablaDeProcesosCM,(void*)limpiarProcesoCM);
	pthread_mutex_unlock(&mutexAccesoMemoria);
}

void limpiarProceso(char* key, t_dictionary *value){
	dictionary_iterator(value,(void*)limpiarPaginaYActualizarSwap);
}

void limpiarProcesoCM(char* key, t_list *value){
	list_clean(value);
}

void limpiarPaginaYActualizarSwap (char* key, pagina_t *value){
	if(value->bitPresencia==1){
			marcos[value->nroMarco]=0;
	}
	value->bitPresencia = 0;
	value->bitUso = 0;
	value->tiempoLRU = 0;
	value->tiempoFIFO = 0;
	if(value->bitModificado==1){
		void* aux = malloc(tamanioMarco);
		memcpy(aux,memoriaPrincipal+value->nroMarco*tamanioMarco,tamanioMarco);
		escribirEnSwap(aux,value->pid,value->nroPagina);//TODO verificar si tira error o no
		free(aux);
	}
	value->nroMarco = -1;
	value->bitModificado = 0;
}

void calculoTasaTLB(){
	int tasaAcierto=0;
	while(1){
		sleep(60);
		if (accesosTLB){
			tasaAcierto=(100*aciertosTLB)/accesosTLB;
			log_info(archivoLog,"La tasa de aciertos de la TLB es: %i \% \n",tasaAcierto);
			log_info(archivoLogObligatorio,"La tasa de aciertos de la TLB es: %i \% \n",tasaAcierto);
		}else{
			log_info(archivoLog,"La tasa de aciertos de la TLB es: NA \n");
			log_info(archivoLogObligatorio,"La tasa de aciertos de la TLB es: NA \n");
		}
	}
}

int hayMarcoLibre(){
	int hayMarco=0,i=0;
	while(i<cantidadMarcos && !hayMarco){
		if(marcos[i]==0){
			hayMarco=1;
		}
		i++;
	}
	return hayMarco;
}
