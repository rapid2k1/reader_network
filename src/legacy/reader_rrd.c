/*
reader_network - A package of utilities to record and work with
multicast radar data in ASTERIX format. (radar as in air navigation
surveillance).

Copyright (C) 2002-2013 Diego Torres <diego dot torres at gmail dot com>

This file is part of the reader_network utils.

reader_network is free software: you can redistribute it and/or modify
it under the terms of the Lesser GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

reader_network is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with reader_network. If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "reader_rrd.h"

#define DEST_FILE_FORMAT_UNKNOWN 1
#define DEST_FILE_FORMAT_AST 2
#define DEST_FILE_FORMAT_GPS 4
#define DEST_FILE_FORMAT_BOTH 6

//date --utc --date "2012-03-02 00:00:00" +%s
// date -d @1193144433
float current_time=0.0;
struct sockaddr_in cliaddr,srvaddr;

bool enabled = false;
bool dest_file_gps = false, source_file_gps = false;
bool dest_file_compress = false;
bool dest_file_timestamp = false;
bool dest_localhost = false;
bool dest_screen_crc = false;
bool mode_daemon = false;
bool mode_scrm = false;
long timed = 0;
time_t midnight_t; //segundos desde el 1-1-1970 hasta las 00:00:00 del dia actual
char *dest_file = NULL, 
    *dest_file_final_ast = NULL, *dest_file_final_gps = NULL, 
    *source_file = NULL, **radar_definition;
char source[] = "file";
int *radar_counter = NULL; // plots recibidos por cada flujo
int *radar_counter_bytes = NULL; // bytes recibidos por cada flujo
int dest_file_format = DEST_FILE_FORMAT_AST;
int radar_count = 0; // numero de entradas en el array de definicion de radares. 
    // para saber el numero de radares, hay que dividir entre 5! (5 columnas por radar)
int socket_count = 0, s, offset = 0;
int fd_in=-1, fd_out_ast=-1,fd_out_gps=-1;
long source_file_gps_version=3;
rb_red_blk_tree* tree = NULL;

struct Queue {
    rb_red_blk_node **node;
    //[MAX_SCRM_SIZE];
    int front, rear;
    int count;
};

struct Queue q;

struct radar_destination_s {
    int socket; // socket descriptor
    char dest_ip[255]; // destination multicast address
};

struct radar_destination_s radar_destination[MAX_RADAR_NUMBER];

void parse_config() {

    enabled = true;
    mode_daemon = false;
    mode_scrm = false;
    timed = 0;
    dest_localhost = true;
    dest_file_timestamp = false;
    dest_file_compress = false;
    dest_file = NULL;
    dest_screen_crc = false;
    source_file_gps_version = 1;
    source_file_gps = true;
    return;
}

void setup_time(long timestamp) {
struct timeval t;
struct tm *t2;

    if (setenv("TZ","UTC",1)==-1) {
	log_printf(LOG_ERROR, "ERROR setenv\n");
	exit(EXIT_FAILURE);
    }
    if (timestamp>0) {
	midnight_t = timestamp;    
	return;
    }
    if (gettimeofday(&t, NULL)==-1) {
	log_printf(LOG_ERROR, "ERROR gettimeofday (setup_time): %s\n", strerror(errno));
	exit(EXIT_FAILURE);
    }
    if ((t2 = gmtime(&t.tv_sec)) == NULL) {
	log_printf(LOG_ERROR, "ERROR gmtime (setup_time): %s\n", strerror(errno));
	exit(EXIT_FAILURE);
    }
    t2->tm_sec = 0; t2->tm_min = 0; t2->tm_hour = 0;
    //log_printf(LOG_ERROR,"DST(%d) (%s) (%ld)\n", t2->tm_isdst,t2->tm_zone,t2->tm_gmtoff);
    if ((midnight_t = mktime(t2))==-1) { //segundos a las 00:00:00 de hoy
	log_printf(LOG_ERROR, "ERROR mktime (setup_time): %s\n", strerror(errno));
	exit(EXIT_FAILURE);
    }
    //log_printf(LOG_ERROR,"DST(%d) (%s) (%ld)\n", t2->tm_isdst,t2->tm_zone,t2->tm_gmtoff);
    //log_printf(LOG_ERROR, "timestamp:%ld\n", midnight_t);
    return;
}

ssize_t setup_input_file(void) {
ssize_t size;

    if ( (fd_in = open(source_file, O_RDONLY)) == -1) {
        log_printf(LOG_ERROR, "ERROR open: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if ( (size = lseek(fd_in, 0, SEEK_END)) == -1) {
        log_printf(LOG_ERROR, "ERROR lseek (seek_end): %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    if ( lseek(fd_in, 0, SEEK_SET) == -1) {
        log_printf(LOG_ERROR, "ERROR lseek (seek_set): %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    return size;
}


void AddQueue(void* a) {
    // printf("add addr:%x crc:%x\n", (unsigned int)a, ((rb_red_blk_node*)a)->crc32);
    if (q.count != MAX_SCRM_SIZE) {
	q.node[q.rear] = (rb_red_blk_node *) a;
	q.rear = (q.rear + 1) % MAX_SCRM_SIZE;
	q.count++;
    }
    return;
}
    
void DeleteQueue(void *a) {
    // printf("delete addr:%x crc:%x\n", (unsigned int)a, ((rb_red_blk_node*)a)->crc32);
    // item = queue.node[queue.front];
    if (q.count != 0) {
	q.front = (q.front + 1) % MAX_SCRM_SIZE;
	q.count--;
    }
}

int UIntComp(unsigned int a, unsigned int b) {
    if (a>b) return (1);
    if (a<b) return (-1);
    return 0;
}


int main(int argc, char *argv[]) {

ssize_t ast_size_total;
ssize_t ast_pos = 0;
ssize_t ast_size_tmp;
int ast_size_datablock;
unsigned char *ast_ptr_raw;
struct timeval timed_t_start; // tiempo inicial para las grabaciones temporizadas
//struct timeval timed_t_current; // tiempo actual de la recepcion del paquete
//struct timeval timed_t_Xsecs; // tiempo inicial de la vuelta actual (para el display de stats)
unsigned long count2_plot_ignored = 0;
unsigned long count2_plot_processed = 0;
//unsigned long count2_plot_unique = 0;
//unsigned long count2_plot_duped = 0;
unsigned long count2_udp_received = 0;
long timestamp = 0;

    mem_open(fail);
    if (log_open(NULL, /*LOG_VERBOSE*/ LOG_NORMAL, /*LOG_TIMESTAMP |*/
	LOG_HAVE_COLORS | LOG_PRINT_FUNCTION |
        LOG_DEBUG_PREFIX_ONLY /*| LOG_DETECT_DUPLICATES*/)) {
        fprintf(stderr, "log_open failed: %m\n");
        exit (EXIT_FAILURE);
    }
    
    memset(full_tod, 0x00, MAX_RADAR_NUMBER*TTOD_WIDTH);
    if (argc>3 || argc<2 || strlen(argv[1])<5) {
        log_printf(LOG_ERROR, "reader_rrd_LNX v%s Copyright (C) 2002 - 2012 Diego Torres\n\n"
            "This program comes with ABSOLUTELY NO WARRANTY.\n"
            "This is free software, and you are welcome to redistribute it\n"
            "under certain conditions; see COPYING file for details.\n\n", VERSION);
	log_printf(LOG_ERROR, "usage: %s <gps_file> [yyyymmdd]\n\n", argv[0]);
	exit(EXIT_FAILURE);
    }
    parse_config(/*argv[1]*/);
//    log_printf(LOG_ERROR, "init...\n");
    if (argc==3) {
	errno = 0;
	timestamp = strtol(argv[2], NULL, 10);
	if (errno!=0) {
	    log_printf(LOG_ERROR, "invalid timestamp (%s)!\n", argv[2]);
	    exit(EXIT_FAILURE);
	}
    }
    setup_time(timestamp);
    setup_crc32_table();
    if (mode_scrm) { 
	tree = RBTreeCreate(UIntComp,AddQueue,DeleteQueue);
	q.node = (rb_red_blk_node **) mem_alloc(sizeof(rb_red_blk_node *) * MAX_SCRM_SIZE);
	q.rear = q.front = q.count = 0;
//	q.node =  mem_alloc(rb_red_blk_node);
    }

    gettimeofday(&timed_t_start, NULL);

    radar_delay_alloc();
    radar_delay_clear();

    source_file = (char *) mem_alloc(strlen(argv[1])+1);
    memcpy(source_file, argv[1], strlen(argv[1])+1);

/*
    {
	unsigned char sac = '\x00',sic = '\x00';
	char * tmp1 = ast_get_SACSIC(&sac,&sic,GET_SAC_SHORT);
	char * tmp2 = ast_get_SACSIC(&sic,&sic,GET_SIC_SHORT);
	printf("sac(%s) sic(%s)\n",tmp1, tmp2);
	//exit(EXIT_FAILURE);
    }
*/

    if (!strncasecmp(source, "file", 4)) { 
	ast_size_total = setup_input_file();
	ast_ptr_raw = (unsigned char *) mem_alloc(ast_size_total);
	if ( (ast_size_tmp = read(fd_in, ast_ptr_raw, ast_size_total)) != ast_size_total) {
	    log_printf(LOG_ERROR, "ERROR read: %s\n", strerror(errno));
	    exit(EXIT_FAILURE);
	}
	//log_printf(LOG_ERROR, "readed %ld bytes\n", (unsigned long) ast_size_total);
        if (source_file_gps_version == 0) {
	    unsigned char *memcmp1;
	    memcmp1 = (unsigned char *) mem_alloc(20);
	    memcmp1 = memset(memcmp1, 0xCD, 20);
	    if (!memcmp(memcmp1, ast_ptr_raw+20, 20)) {
	        offset = 10; ast_pos += 2200; source_file_gps_version = 1;
	        //log_printf(LOG_ERROR, "GPSv1 input auto-activated\n");
	    } else {
	        offset = 4; ast_pos = 0; source_file_gps_version = 2;
	        //log_printf(LOG_ERROR, "GPSv2 input auto-activated\n");
	    }
	    mem_free(memcmp1);
	} else {
	    if (source_file_gps_version == 1)
		ast_pos += 2200; offset = 10;
	}

	while (ast_pos < ast_size_total) {
	    ast_size_datablock = (ast_ptr_raw[ast_pos + 1]<<8) + ast_ptr_raw[ast_pos + 2];
	    count2_udp_received++;

	    if (source_file_gps) {
		if (source_file_gps_version == 1) {
		    current_time = ((ast_ptr_raw[ast_pos + ast_size_datablock + 6]<<16 ) +
			(ast_ptr_raw[ast_pos + ast_size_datablock + 7] << 8) +
			(ast_ptr_raw[ast_pos + ast_size_datablock + 8]) ) / 128.0;
		} else if (source_file_gps_version == 2) {
		    current_time = ((ast_ptr_raw[ast_pos + ast_size_datablock] ) +
			(ast_ptr_raw[ast_pos + ast_size_datablock + 1] << 8) +
			(ast_ptr_raw[ast_pos + ast_size_datablock + 2] << 16) +
			(ast_ptr_raw[ast_pos + ast_size_datablock + 3] << 24) ) / 1000.0;
		}
	    } 

	    //log_printf(LOG_VERBOSE, "%ld %d %3.3f plots_processed(%ld) plots_ignored(%ld) cat(%02X)\n", (long int)ast_pos, ast_size_datablock, 
		//current_time, count2_plot_processed, count2_plot_ignored, ast_ptr_raw[ast_pos]);
	    if (dest_localhost) {
//		int l;
		
		ast_output_datablock(ast_ptr_raw + ast_pos, ast_size_datablock/* + offset*/, count2_plot_processed+1, 0);

//		for (l=0; l < ast_size_datablock; l++)
//		    printf("[%02X]", (unsigned char) ast_ptr_raw[ast_pos + l]);
//		printf("\n");
		         
		if (ast_ptr_raw[ast_pos] == '\x01') {
		    count2_plot_processed++;
		    ast_procesarCAT01(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x02') {
		    count2_plot_processed++;
		    ast_procesarCAT02(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x08') {
		    count2_plot_processed++;
		    ast_procesarCAT08(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x0a') {
		    count2_plot_processed++;
		    ast_procesarCAT10(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x13') {
		    count2_plot_processed++;
		    ast_procesarCAT19(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x14') {
		    count2_plot_processed++;
		    ast_procesarCAT20(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x15') {
		    count2_plot_processed++;
		    ast_procesarCAT21(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x22') {
		    count2_plot_processed++;
		    ast_procesarCAT34(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x30') {
		    count2_plot_processed++;
		    ast_procesarCAT48(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else if (ast_ptr_raw[ast_pos] == '\x3e') {
		    count2_plot_processed++;
		    ast_procesarCAT62(ast_ptr_raw + ast_pos + 3, ast_size_datablock, count2_plot_processed, false);
		} else {
		    count2_plot_ignored++;
		}
	    }
	    ast_pos += ast_size_datablock + offset;
//	    usleep(100);
	}
	mem_free(ast_ptr_raw);

    } 

    if (dest_localhost) { // if sending decoded asterix, tell clients that we are closing!
	struct datablock_plot dbp;
	dbp.cat = CAT_255; dbp.available = IS_ERROR;
	update_calculations(dbp);
//	if (sendto(s, &dbp, sizeof(dbp), 0, (struct sockaddr *) &srvaddr, sizeof(srvaddr)) < 0) {
//	    log_printf(LOG_ERROR, "ERROR sendto: %s\n", strerror(errno));
//	}
    }

    log_flush();
//    log_printf(LOG_ERROR, "stats received[%ld] processed[%ld]/ignored[%ld] = unique[%ld]+duped[%ld]\n", 
//	count2_udp_received, count2_plot_processed, count2_plot_ignored, count2_plot_unique, count2_plot_duped);



//    log_flush();
    if (mode_scrm) {
        mem_free(q.node);
	RBTreeDestroy(tree);
    }
    close(s);
    close(fd_in);
//    close(fd_out_ast);
//    close(fd_out_gps);
//    close_output_file();
    return 0;

}

#define SERVER_TIMEOUT_SEC 10

struct sorted_list {
    double segment;
    int count;
    struct sorted_list *next;
};

struct radar_delay_s {
    unsigned char sac,sic;
    unsigned char first_time[256];
    long cuenta_plot_cat1, cuenta_plot_cat2;
    long cuenta_plot_cat8, cuenta_plot_cat10;
    long cuenta_plot_cat19, cuenta_plot_cat20;
    long cuenta_plot_cat21;
    long cuenta_plot_cat34, cuenta_plot_cat48;
    double suma_retardos_cat1, suma_retardos_cat2;
    double suma_retardos_cat8, suma_retardos_cat10;
    double suma_retardos_cat19, suma_retardos_cat20;
    double suma_retardos_cat21;
    double suma_retardos_cat34, suma_retardos_cat48;
    double suma_retardos_cuad_cat1, suma_retardos_cuad_cat2;
    double suma_retardos_cuad_cat8, suma_retardos_cuad_cat10;
    double suma_retardos_cuad_cat19, suma_retardos_cuad_cat20;
    double suma_retardos_cuad_cat21;
    double suma_retardos_cuad_cat34, suma_retardos_cuad_cat48;
    double max_retardo_cat1, max_retardo_cat2;
    double max_retardo_cat8, max_retardo_cat10;
    double max_retardo_cat19, max_retardo_cat20;
    double max_retardo_cat21;
    double max_retardo_cat34, max_retardo_cat48;
    double min_retardo_cat1, min_retardo_cat2;
    double min_retardo_cat8, min_retardo_cat10;
    double min_retardo_cat19, min_retardo_cat20;
    double min_retardo_cat21;
    double min_retardo_cat34, min_retardo_cat48;
    int *segmentos_cat1, *segmentos_cat2;
    int *segmentos_cat8, *segmentos_cat10;
    int *segmentos_cat19, *segmentos_cat20;
    int *segmentos_cat21;
    int *segmentos_cat34, *segmentos_cat48;
    int segmentos_max_cat1, segmentos_max_cat2;
    int segmentos_max_cat8, segmentos_max_cat10;
    int segmentos_max_cat19, segmentos_max_cat20;
    int segmentos_max_cat21;
    int segmentos_max_cat34, segmentos_max_cat48;
    int segmentos_ptr_cat1, segmentos_ptr_cat2;
    int segmentos_ptr_cat8, segmentos_ptr_cat10;
    int segmentos_ptr_cat19, segmentos_ptr_cat20;
    int segmentos_ptr_cat21;
    int segmentos_ptr_cat34, segmentos_ptr_cat48;
    struct sorted_list *sorted_list_cat1, *sorted_list_cat2;
    struct sorted_list *sorted_list_cat8, *sorted_list_cat10;
    struct sorted_list *sorted_list_cat19, *sorted_list_cat20;
    struct sorted_list *sorted_list_cat21;
    struct sorted_list *sorted_list_cat34, *sorted_list_cat48;
};

struct radar_delay_s *radar_delay;

struct ip_mreq mreq;
struct sockaddr_in addr;
fd_set reader_set;
int s, yes = 1;
bool forced_exit = false;

void insertList(struct sorted_list **p, int segment, int count) {

    double fsegment = (segment * 0.005) - 8;
//    log_printf(LOG_NORMAL, "0) insertando(%d) ROOT(%08X)\n", count, (unsigned int)*p);
    if (*p==NULL) {
	*p = (struct sorted_list *) mem_alloc(sizeof(struct sorted_list));
	(*p)->segment = fsegment;
	(*p)->count = count;
	(*p)->next = NULL;
//	log_printf(LOG_NORMAL, "1)%d ROOT(%08X)\n", (*p)->count, (unsigned int)(*p));
    } else { // ordenaremos de menor a mayor
	struct sorted_list *t = *p;
	struct sorted_list *nuevo;
	struct sorted_list *old = NULL;

//	log_printf(LOG_NORMAL, "2)%d %08X\n", t->count, (unsigned int)t);
	
//	int i=0;
	while((fsegment > t->segment) && (t->next!=NULL)) {
//	    log_printf(LOG_NORMAL, "%d\n", i++);
	    old = t;
	    t = t->next;
	}
//        log_printf(LOG_NORMAL, "3)ROOT(%08X) t(%08X)\n",(unsigned int) *p, (unsigned int) t);

	nuevo = (struct sorted_list *) mem_alloc(sizeof(struct sorted_list));
	nuevo->count = count;
	nuevo->segment = fsegment;
	nuevo->next = NULL;

	if (fsegment > t->segment) { // se da de alta detras del elemento actual
	    nuevo->next = t->next;
	    t->next = nuevo;
//	    log_printf(LOG_NORMAL, "4)\n");
	} else {
	    if (fsegment <= t->segment && (t!=*p)) { // se da de alta en lugar del elemento actual, pero no es ppio de lista
		old->next = nuevo;
		nuevo->next = t;
//	        log_printf(LOG_NORMAL, "5)\n");
	    }
	    if (t == *p) { // insertando al principio de la lista
		nuevo->next = *p; 
		*p = nuevo;
//		log_printf(LOG_NORMAL, "6)\n");
	    }
	}
//	log_printf(LOG_NORMAL, "7)%d %08X\n", nuevo->count, (unsigned int)nuevo);
    }
//    log_printf(LOG_NORMAL, "8)ROOT(%08X)\n\n", (unsigned int)*p);
    return;
}

void radar_delay_alloc(void) {
int i,j;

    radar_delay = (struct radar_delay_s *) mem_alloc(sizeof(struct radar_delay_s)*MAX_RADAR_NUMBER);
    for (i=0; i < MAX_RADAR_NUMBER; i++) {
	radar_delay[i].segmentos_cat1 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat2 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat8 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat10 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat19 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat20 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat21 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat34 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].segmentos_cat48 = (int *) mem_alloc(sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].sorted_list_cat1 = radar_delay[i].sorted_list_cat2 = NULL;
	radar_delay[i].sorted_list_cat8 = radar_delay[i].sorted_list_cat10 = NULL;
	radar_delay[i].sorted_list_cat19 = radar_delay[i].sorted_list_cat20 = NULL;
	radar_delay[i].sorted_list_cat21 = NULL;
	radar_delay[i].sorted_list_cat34 = radar_delay[i].sorted_list_cat48 = NULL;
	for(j=0;j<256;j++) radar_delay[i].first_time[j] = 0;
    }
    return;
}

void radar_delay_clear(void) {
int i,j;
    for (i=0; i < MAX_RADAR_NUMBER; i++) {
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat1;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat2;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat8;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat10;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat19;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat20;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat21;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat34;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	{
	    struct sorted_list *p = radar_delay[i].sorted_list_cat48;
	    while (p!=NULL) { struct sorted_list *p2 = p->next; mem_free(p); p = p2; }
	}
	radar_delay[i].sorted_list_cat1 = radar_delay[i].sorted_list_cat2 = NULL;
	radar_delay[i].sorted_list_cat8 = radar_delay[i].sorted_list_cat10 = NULL;
	radar_delay[i].sorted_list_cat19 = radar_delay[i].sorted_list_cat20 = NULL;
	radar_delay[i].sorted_list_cat21 = NULL;
	radar_delay[i].sorted_list_cat34 = radar_delay[i].sorted_list_cat48 = NULL;
	
	bzero(radar_delay[i].segmentos_cat1, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat2, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat8, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat10, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat19, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat20, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat21, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat34, sizeof(int) * MAX_SEGMENT_NUMBER);
	bzero(radar_delay[i].segmentos_cat48, sizeof(int) * MAX_SEGMENT_NUMBER);
	radar_delay[i].sac = '\0'; radar_delay[i].sic = '\0';
	for(j=0;j<256;j++) radar_delay[i].first_time[j] = 0;
	radar_delay[i].cuenta_plot_cat1 = 0; radar_delay[i].cuenta_plot_cat2 = 0;
	radar_delay[i].cuenta_plot_cat8 = 0; radar_delay[i].cuenta_plot_cat10 = 0;
	radar_delay[i].cuenta_plot_cat19 = 0; radar_delay[i].cuenta_plot_cat20 = 0;
	radar_delay[i].cuenta_plot_cat21 = 0;
	radar_delay[i].cuenta_plot_cat34 = 0; radar_delay[i].cuenta_plot_cat48 = 0;
	radar_delay[i].suma_retardos_cat1 = 0; radar_delay[i].suma_retardos_cat2 = 0;
	radar_delay[i].suma_retardos_cat8 = 0; radar_delay[i].suma_retardos_cat10 = 0;
	radar_delay[i].suma_retardos_cat19 = 0; radar_delay[i].suma_retardos_cat20 = 0;
	radar_delay[i].suma_retardos_cat21 = 0;
	radar_delay[i].suma_retardos_cat34 = 0; radar_delay[i].suma_retardos_cat48 = 0;
	radar_delay[i].suma_retardos_cuad_cat1 = 0; radar_delay[i].suma_retardos_cuad_cat2 = 0;
	radar_delay[i].suma_retardos_cuad_cat8 = 0; radar_delay[i].suma_retardos_cuad_cat10 = 0;
	radar_delay[i].suma_retardos_cuad_cat19 = 0; radar_delay[i].suma_retardos_cuad_cat20 = 0;
	radar_delay[i].suma_retardos_cuad_cat21 = 0;
	radar_delay[i].suma_retardos_cuad_cat34 = 0; radar_delay[i].suma_retardos_cuad_cat48 = 0;
	radar_delay[i].max_retardo_cat1 = -10000.0; radar_delay[i].max_retardo_cat2 = -10000.0;
	radar_delay[i].max_retardo_cat8 = -10000.0; radar_delay[i].max_retardo_cat10 = -10000.0;
	radar_delay[i].max_retardo_cat19 = -10000.0; radar_delay[i].max_retardo_cat20 = -10000.0;
	radar_delay[i].max_retardo_cat21 = -10000.0;
	radar_delay[i].max_retardo_cat34 = -10000.0; radar_delay[i].max_retardo_cat48 = -10000.0;
	radar_delay[i].min_retardo_cat1 = 10000.0; radar_delay[i].min_retardo_cat2 = 10000.0;
	radar_delay[i].min_retardo_cat8 = 10000.0; radar_delay[i].min_retardo_cat10 = 10000.0;
	radar_delay[i].min_retardo_cat19 = 10000.0; radar_delay[i].min_retardo_cat20 = 10000.0;
	radar_delay[i].min_retardo_cat21 = 10000.0;
	radar_delay[i].min_retardo_cat34 = 10000.0; radar_delay[i].min_retardo_cat48 = 10000.0;
	radar_delay[i].segmentos_max_cat1 = 0; radar_delay[i].segmentos_max_cat2 = 0;
	radar_delay[i].segmentos_max_cat8 = 0; radar_delay[i].segmentos_max_cat10 = 0;
	radar_delay[i].segmentos_max_cat19 = 0; radar_delay[i].segmentos_max_cat20 = 0;
	radar_delay[i].segmentos_max_cat21 = 0;
	radar_delay[i].segmentos_max_cat34 = 0; radar_delay[i].segmentos_max_cat48 = 0;
	radar_delay[i].segmentos_ptr_cat1 = 0; radar_delay[i].segmentos_ptr_cat2 = 0;
	radar_delay[i].segmentos_ptr_cat8 = 0; radar_delay[i].segmentos_ptr_cat10 = 0;
	radar_delay[i].segmentos_ptr_cat19 = 0; radar_delay[i].segmentos_ptr_cat20 = 0;
	radar_delay[i].segmentos_ptr_cat21 = 0;
	radar_delay[i].segmentos_ptr_cat34 = 0; radar_delay[i].segmentos_ptr_cat48 = 0;
    }
}

void radar_delay_free(void) {
int i;

    for (i=0; i < MAX_RADAR_NUMBER; i++) {
	mem_free(radar_delay[i].segmentos_cat1);
	mem_free(radar_delay[i].segmentos_cat2);
	mem_free(radar_delay[i].segmentos_cat8);
	mem_free(radar_delay[i].segmentos_cat10);
	mem_free(radar_delay[i].segmentos_cat19);
	mem_free(radar_delay[i].segmentos_cat20);
	mem_free(radar_delay[i].segmentos_cat21);
	mem_free(radar_delay[i].segmentos_cat34);
	mem_free(radar_delay[i].segmentos_cat48);
    }
    mem_free(radar_delay);
}

#define FIRST_STEP DBL_MAX
double step = FIRST_STEP;
double last_tod_stamp;
void update_calculations(struct datablock_plot dbp) {
double diff = 0.0, stdev = 0.0, media = 0.0;
div_t d;

    if (dbp.cat == CAT_255) {
	//log_printf(LOG_ERROR, "fin de fichero\n");
	forced_exit = true;
    }

    //log_printf(LOG_VERBOSE, "hola0 %3.3f %d %d %c\n", step,dbp.sac, dbp.sic, dbp.cat);

    if (dbp.available & IS_TOD) {
	diff = dbp.tod_stamp - dbp.tod;
	if (diff<=-86000) {
	    diff+=86400; // rollover tod correction
	} else if (diff>=(86400-512)) {
	    diff-=86400; // rollover tod correction
	}
        d = div( dbp.tod_stamp, UPDATE_TIME_RRD);
	if (step == FIRST_STEP) {
    	    step = (d.quot * UPDATE_TIME_RRD + UPDATE_TIME_RRD) - 1.0/2048.0 + midnight_t;
	}
	//log_printf(LOG_VERBOSE, "sac:%d sic:%d tod:%3.3f tod_stamp:%3.3f diff:%3.3f\n", dbp.sac, dbp.sic, dbp.tod, dbp.tod_stamp, diff);
	//printf("sac:%d sic:%d tod:%s tod_stamp:%s diff:%3.3f\n", dbp.sac, dbp.sic, parse_hora(dbp.tod), parse_hora(dbp.tod_stamp), diff);
	//log_printf(LOG_VERBOSE, "%3.7f\n", d.quot*UPDATE_TIME_RRD + UPDATE_TIME_RRD);
	//log_printf(LOG_VERBOSE, "d.quot(%d) d.rem(%d) tod_stamp(%3.3f)=>(%s)+midnight_t(%ld) step(%3.6f) UPDATE_TIME_RRD(%3.3f)\n",d.quot, d.rem, dbp.tod_stamp, parse_hora(dbp.tod_stamp), (long)midnight_t, step, UPDATE_TIME_RRD);
    }



	
    //printf("tod:%ld mod:%03d b:%d\n",t.tv_sec, d.rem, write_stats); printf("\033[1A");
    if (forced_exit || (step<FIRST_STEP && ((dbp.tod_stamp + midnight_t) > step)) ) {
	int i,j;
	char *sac_s=0, *sic_l=0;
	double l1=0.0, l2=0.0, l8=0.0, l10=0.0;
	double l21=0.0, l34=0.0, l48=0.0;
	double sc1_1=0.0, sc1_2=0.0, sc1_3=0.0;
	double sc2_1=0.0, sc2_2=0.0, sc2_3=0.0;
	double sc8_1=0.0, sc8_2=0.0, sc8_3=0.0;
	double sc10_1=0.0, sc10_2=0.0, sc10_3=0.0;
	double sc21_1=0.0, sc21_2=0.0, sc21_3=0.0;
	double sc34_1=0.0, sc34_2=0.0, sc34_3=0.0;
	double sc48_1=0.0, sc48_2=0.0, sc48_3=0.0;
	double moda=0.0, p99_cat1=0.0, p99_cat2=0.0;
	double p99_cat8=0.0, p99_cat10=0.0, p99_cat21=0.0;
	double p99_cat34=0.0, p99_cat48=0.0;
    
	if (!forced_exit) {
	    step = (d.quot * UPDATE_TIME_RRD + UPDATE_TIME_RRD) - 1.0/2048.0 + midnight_t;
	    last_tod_stamp = dbp.tod_stamp;
	} else {
	    dbp.tod_stamp = last_tod_stamp + UPDATE_TIME_RRD;
	}
    
	//log_printf(LOG_NORMAL, "RADAR CAT timestamp    plts media dsv   moda   max   min   p99\n");

	for(i=0; i<MAX_RADAR_NUMBER; i++) {
	    if (radar_delay[i].sac || radar_delay[i].sic) {
	        sac_s = ast_get_SACSIC((unsigned char *) &radar_delay[i].sac,
	    	    (unsigned char *) &radar_delay[i].sic, GET_SAC_SHORT);
		sic_l = ast_get_SACSIC((unsigned char *) &radar_delay[i].sac, 
		    (unsigned char *) &radar_delay[i].sic, GET_SIC_LONG);

		for(j=0; j<MAX_SEGMENT_NUMBER; j++) {
		    if (radar_delay[i].cuenta_plot_cat1>0) {
		        if (radar_delay[i].segmentos_cat1[j] > radar_delay[i].segmentos_max_cat1) {
			    radar_delay[i].segmentos_ptr_cat1 = j;
			    radar_delay[i].segmentos_max_cat1 = radar_delay[i].segmentos_cat1[j];
			}
			if (radar_delay[i].segmentos_cat1[j]>0)
			    insertList(&radar_delay[i].sorted_list_cat1, j, radar_delay[i].segmentos_cat1[j]);
		    }
		    if (radar_delay[i].cuenta_plot_cat2>0) {
		        if (radar_delay[i].segmentos_cat2[j] > radar_delay[i].segmentos_max_cat2) {
			    radar_delay[i].segmentos_ptr_cat2 = j;
			    radar_delay[i].segmentos_max_cat2 = radar_delay[i].segmentos_cat2[j];
			}
			if (radar_delay[i].segmentos_cat2[j]>0)
				insertList(&radar_delay[i].sorted_list_cat2, j, radar_delay[i].segmentos_cat2[j]);
		    }
		    if (radar_delay[i].cuenta_plot_cat8>0) {
		        if (radar_delay[i].segmentos_cat8[j] > radar_delay[i].segmentos_max_cat8) {
			    radar_delay[i].segmentos_ptr_cat8 = j;
			    radar_delay[i].segmentos_max_cat8 = radar_delay[i].segmentos_cat8[j];
			}
			if (radar_delay[i].segmentos_cat8[j]>0)
			    insertList(&radar_delay[i].sorted_list_cat8, j, radar_delay[i].segmentos_cat8[j]);
		    }
		    if (radar_delay[i].cuenta_plot_cat10>0) {
			if (radar_delay[i].segmentos_cat10[j] > radar_delay[i].segmentos_max_cat10) {
			    radar_delay[i].segmentos_ptr_cat10 = j;
			    radar_delay[i].segmentos_max_cat10 = radar_delay[i].segmentos_cat10[j];
			}
			if (radar_delay[i].segmentos_cat10[j]>0)
			    insertList(&radar_delay[i].sorted_list_cat10, j, radar_delay[i].segmentos_cat10[j]);
		    }
		    if (radar_delay[i].cuenta_plot_cat21>0) {
			if (radar_delay[i].segmentos_cat21[j] > radar_delay[i].segmentos_max_cat21) {
			    radar_delay[i].segmentos_ptr_cat21 = j;
			    radar_delay[i].segmentos_max_cat21 = radar_delay[i].segmentos_cat21[j];
			}
			if (radar_delay[i].segmentos_cat21[j]>0)
			    insertList(&radar_delay[i].sorted_list_cat21, j, radar_delay[i].segmentos_cat21[j]);
		    }
		    if (radar_delay[i].cuenta_plot_cat34>0) {
		        if (radar_delay[i].segmentos_cat34[j] > radar_delay[i].segmentos_max_cat34) {
			    radar_delay[i].segmentos_ptr_cat34 = j;
			    radar_delay[i].segmentos_max_cat34 = radar_delay[i].segmentos_cat34[j];
			}
			if (radar_delay[i].segmentos_cat34[j]>0)
			    insertList(&radar_delay[i].sorted_list_cat34, j, radar_delay[i].segmentos_cat34[j]);
		    }
		    if (radar_delay[i].cuenta_plot_cat48>0) {
		        if (radar_delay[i].segmentos_cat48[j] > radar_delay[i].segmentos_max_cat48) {
			    radar_delay[i].segmentos_ptr_cat48 = j;
			    radar_delay[i].segmentos_max_cat48 = radar_delay[i].segmentos_cat48[j];
			}
			if (radar_delay[i].segmentos_cat48[j]>0)
			    insertList(&radar_delay[i].sorted_list_cat48, j, radar_delay[i].segmentos_cat48[j]);
		    }
		}
		    
		// 1
		if (radar_delay[i].cuenta_plot_cat1>0) {
		    l1 =    ( ((double) radar_delay[i].segmentos_ptr_cat1) / 10000.0*50.0 ) - 8.0;
		    sc1_1 = ( ((double) radar_delay[i].segmentos_cat1[radar_delay[i].segmentos_ptr_cat1]) / 10000.0*50.0 ) - 8.0;
		    sc1_2 = ( ((double) radar_delay[i].segmentos_cat1[radar_delay[i].segmentos_ptr_cat1 + 1]) / 10000.0*50.0 ) - 8.0;
		    sc1_3 = ( ((double) radar_delay[i].segmentos_cat1[radar_delay[i].segmentos_ptr_cat1 - 1]) / 10000.0*50.0 ) - 8.0;
		    if(radar_delay[i].sorted_list_cat1!=NULL) {
		        struct sorted_list *p = radar_delay[i].sorted_list_cat1; struct sorted_list *p_old = NULL;
			long count = 0; double count_percentil_99 = ((double)radar_delay[i].cuenta_plot_cat1)*99.0/100.0;
			while(p!=NULL) {
			    if ((count + p->count)>=count_percentil_99) { break; }
			    count += p->count; p_old = p; p = p->next;
			}
			if (p!=NULL && p_old!=NULL) { p99_cat1 = p->segment; }
		    }
		    media = radar_delay[i].suma_retardos_cat1/radar_delay[i].cuenta_plot_cat1;
		    moda = l1 + ( (sc1_1 - sc1_3) / ( (sc1_1 - sc1_3) + (sc1_1 - sc1_2) ) ) * 0.005;
		    stdev = sqrt((radar_delay[i].suma_retardos_cuad_cat1 / radar_delay[i].cuenta_plot_cat1) - 
			pow(radar_delay[i].suma_retardos_cat1 / radar_delay[i].cuenta_plot_cat1,2));
		    update_RRD(radar_delay[i].sac, radar_delay[i].sic, 1, i,
			((long) dbp.tod_stamp) + midnight_t,
			radar_delay[i].cuenta_plot_cat1, radar_delay[i].max_retardo_cat1, radar_delay[i].min_retardo_cat1,
    			media, stdev, moda, p99_cat1);
		}
		// 2
		if (radar_delay[i].cuenta_plot_cat2>0) {
		    l2 =    ( ((double) radar_delay[i].segmentos_ptr_cat2) / 10000.0*50.0 ) - 8.0;
		    sc2_1 = ( ((double) radar_delay[i].segmentos_cat2[radar_delay[i].segmentos_ptr_cat2]) / 10000.0*50.0 ) - 8.0;
		    sc2_2 = ( ((double) radar_delay[i].segmentos_cat2[radar_delay[i].segmentos_ptr_cat2 + 1]) / 10000.0*50.0 ) - 8.0;
		    sc2_3 = ( ((double) radar_delay[i].segmentos_cat2[radar_delay[i].segmentos_ptr_cat2 - 1]) / 10000.0*50.0 ) - 8.0;
		    if(radar_delay[i].sorted_list_cat2!=NULL) {
		        struct sorted_list *p = radar_delay[i].sorted_list_cat2; struct sorted_list *p_old = NULL;
			long count = 0; double count_percentil_99 = ((double)radar_delay[i].cuenta_plot_cat2)*99.0/100.0;
			while(p!=NULL) {
			    if ((count + p->count)>count_percentil_99) { break; }
			    count += p->count; p_old = p; p = p->next;
			}
			if (p!=NULL && p_old!=NULL) { p99_cat2 = p->segment; }
		    }
		    media = radar_delay[i].suma_retardos_cat2/radar_delay[i].cuenta_plot_cat2;
		    moda = l2 + ( (sc2_1 - sc2_3) / ( (sc2_1 - sc2_3) + (sc2_1 - sc2_2) ) ) * 0.005;
		    stdev = sqrt((radar_delay[i].suma_retardos_cuad_cat2 / radar_delay[i].cuenta_plot_cat2) - 
			pow(radar_delay[i].suma_retardos_cat2 / radar_delay[i].cuenta_plot_cat2,2));
		    update_RRD(radar_delay[i].sac, radar_delay[i].sic, 2, i,
			((long) dbp.tod_stamp) + midnight_t,
			radar_delay[i].cuenta_plot_cat2, radar_delay[i].max_retardo_cat2, radar_delay[i].min_retardo_cat2,
    			media, stdev, moda, p99_cat2);
		}
		// 8
		if (radar_delay[i].cuenta_plot_cat8>0) {
		    l8 =    ( ((double) radar_delay[i].segmentos_ptr_cat8) / 10000.0*50.0 ) - 8.0;
		    sc8_1 = ( ((double) radar_delay[i].segmentos_cat8[radar_delay[i].segmentos_ptr_cat8]) / 10000.0*50.0 ) - 8.0;
		    sc8_2 = ( ((double) radar_delay[i].segmentos_cat8[radar_delay[i].segmentos_ptr_cat8 + 1]) / 10000.0*50.0 ) - 8.0;
		    sc8_3 = ( ((double) radar_delay[i].segmentos_cat8[radar_delay[i].segmentos_ptr_cat8 - 1]) / 10000.0*50.0 ) - 8.0;
		    if(radar_delay[i].sorted_list_cat8!=NULL) {
		        struct sorted_list *p = radar_delay[i].sorted_list_cat8; struct sorted_list *p_old = NULL;
			long count = 0; double count_percentil_99 = ((double)radar_delay[i].cuenta_plot_cat8)*99.0/100.0;
			while(p!=NULL) {
			    if ((count + p->count)>count_percentil_99) { break; }
			    count += p->count; p_old = p; p = p->next;
			}
			if (p!=NULL && p_old!=NULL) { p99_cat8 = p->segment; }
		    }
		    media = radar_delay[i].suma_retardos_cat8/radar_delay[i].cuenta_plot_cat8;
		    moda = l8 + ( (sc8_1 - sc8_3) / ( (sc8_1 - sc8_3) + (sc8_1 - sc8_2) ) ) * 0.005;
		    stdev = sqrt((radar_delay[i].suma_retardos_cuad_cat8 / radar_delay[i].cuenta_plot_cat8) - 
			pow(radar_delay[i].suma_retardos_cat8 / radar_delay[i].cuenta_plot_cat8,2));
		    update_RRD(radar_delay[i].sac, radar_delay[i].sic, 8, i,
			((long) dbp.tod_stamp) + midnight_t,
			radar_delay[i].cuenta_plot_cat8, radar_delay[i].max_retardo_cat8, radar_delay[i].min_retardo_cat8,
    			media, stdev, moda, p99_cat8);
		}
		// 10
		if (radar_delay[i].cuenta_plot_cat10>0) {
		    l10 =    ( ((double) radar_delay[i].segmentos_ptr_cat10) / 10000.0*50.0 ) - 8.0;
		    sc10_1 = ( ((double) radar_delay[i].segmentos_cat10[radar_delay[i].segmentos_ptr_cat10]) / 10000.0*50.0 ) - 8.0;
		    sc10_2 = ( ((double) radar_delay[i].segmentos_cat10[radar_delay[i].segmentos_ptr_cat10 + 1]) / 10000.0*50.0 ) - 8.0;
		    sc10_3 = ( ((double) radar_delay[i].segmentos_cat10[radar_delay[i].segmentos_ptr_cat10 - 1]) / 10000.0*50.0 ) - 8.0;
		    if(radar_delay[i].sorted_list_cat10!=NULL) {
		        struct sorted_list *p = radar_delay[i].sorted_list_cat10; struct sorted_list *p_old = NULL;
			long count = 0; double count_percentil_99 = ((double)radar_delay[i].cuenta_plot_cat10)*99.0/100.0;
			while(p!=NULL) {
			    if ((count + p->count)>count_percentil_99) { break; }
			    count += p->count; p_old = p; p = p->next;
			}
			if (p!=NULL && p_old!=NULL) { p99_cat10 = p->segment; }
		    }
		    media = radar_delay[i].suma_retardos_cat10/radar_delay[i].cuenta_plot_cat10;
		    moda = l10 + ( (sc10_1 - sc10_3) / ( (sc10_1 - sc10_3) + (sc10_1 - sc10_2) ) ) * 0.005;
		    stdev = sqrt((radar_delay[i].suma_retardos_cuad_cat10 / radar_delay[i].cuenta_plot_cat10) - 
			pow(radar_delay[i].suma_retardos_cat10 / radar_delay[i].cuenta_plot_cat10,2));
		    update_RRD(radar_delay[i].sac, radar_delay[i].sic, 10, i,
			((long) dbp.tod_stamp) + midnight_t,
			radar_delay[i].cuenta_plot_cat10, radar_delay[i].max_retardo_cat10, radar_delay[i].min_retardo_cat10,
    			media, stdev, moda, p99_cat10);
		}
		// 21
		if (radar_delay[i].cuenta_plot_cat21>0) {
		    l21 =    ( ((double) radar_delay[i].segmentos_ptr_cat21) / 10000.0*50.0 ) - 8.0;
		    sc21_1 = ( ((double) radar_delay[i].segmentos_cat21[radar_delay[i].segmentos_ptr_cat21]) / 10000.0*50.0 ) - 8.0;
		    sc21_2 = ( ((double) radar_delay[i].segmentos_cat21[radar_delay[i].segmentos_ptr_cat21 + 1]) / 10000.0*50.0 ) - 8.0;
		    sc21_3 = ( ((double) radar_delay[i].segmentos_cat21[radar_delay[i].segmentos_ptr_cat21 - 1]) / 10000.0*50.0 ) - 8.0;
		    if(radar_delay[i].sorted_list_cat21!=NULL) {
		        struct sorted_list *p = radar_delay[i].sorted_list_cat21; struct sorted_list *p_old = NULL;
		        long count = 0; double count_percentil_99 = ((double)radar_delay[i].cuenta_plot_cat21)*99.0/100.0;
			while(p!=NULL) {
			    if ((count + p->count)>count_percentil_99) { break; }
			    count += p->count; p_old = p; p = p->next;
			}
			if (p!=NULL && p_old!=NULL) { p99_cat21 = p->segment; }
		    }
		    media = radar_delay[i].suma_retardos_cat21/radar_delay[i].cuenta_plot_cat21;
		    moda = l21 + ( (sc21_1 - sc21_3) / ( (sc21_1 - sc21_3) + (sc21_1 - sc21_2) ) ) * 0.005;
		    stdev = sqrt((radar_delay[i].suma_retardos_cuad_cat21 / radar_delay[i].cuenta_plot_cat21) - 
			pow(radar_delay[i].suma_retardos_cat21 / radar_delay[i].cuenta_plot_cat21,2));
		    update_RRD(radar_delay[i].sac, radar_delay[i].sic, 21, i,
			((long) dbp.tod_stamp) + midnight_t,
			radar_delay[i].cuenta_plot_cat21, radar_delay[i].max_retardo_cat21, radar_delay[i].min_retardo_cat21,
    			media, stdev, moda, p99_cat21);

		}
		// 34
		if (radar_delay[i].cuenta_plot_cat34>0) {
		    l34 =    ( ((double) radar_delay[i].segmentos_ptr_cat34) / 10000.0*50.0 ) - 8.0;
		    sc34_1 = ( ((double) radar_delay[i].segmentos_cat34[radar_delay[i].segmentos_ptr_cat34]) / 10000.0*50.0 ) - 8.0;
		    sc34_2 = ( ((double) radar_delay[i].segmentos_cat34[radar_delay[i].segmentos_ptr_cat34 + 1]) / 10000.0*50.0 ) - 8.0;
		    sc34_3 = ( ((double) radar_delay[i].segmentos_cat34[radar_delay[i].segmentos_ptr_cat34 - 1]) / 10000.0*50.0 ) - 8.0;
		    if(radar_delay[i].sorted_list_cat34!=NULL) {
		        struct sorted_list *p = radar_delay[i].sorted_list_cat34; struct sorted_list *p_old = NULL;
			long count = 0; double count_percentil_99 = ((double)radar_delay[i].cuenta_plot_cat34)*99.0/100.0;
			while(p!=NULL) {
			    if ((count + p->count)>count_percentil_99) { break; }
			    count += p->count; p_old = p; p = p->next;
			}
			if (p!=NULL && p_old!=NULL) { p99_cat34 = p->segment; }
		    }
		    media = radar_delay[i].suma_retardos_cat34/radar_delay[i].cuenta_plot_cat34;
		    moda = l34 + ( (sc34_1 - sc34_3) / ( (sc34_1 - sc34_3) + (sc34_1 - sc34_2) ) ) * 0.005;
		    stdev = sqrt((radar_delay[i].suma_retardos_cuad_cat34 / radar_delay[i].cuenta_plot_cat34) - 
			pow(radar_delay[i].suma_retardos_cat34 / radar_delay[i].cuenta_plot_cat34,2));
		    update_RRD(radar_delay[i].sac, radar_delay[i].sic, 34, i,
			((long) dbp.tod_stamp) + midnight_t,
			radar_delay[i].cuenta_plot_cat34, radar_delay[i].max_retardo_cat34, radar_delay[i].min_retardo_cat34,
    			media, stdev, moda, p99_cat34);
		}
		// 48
		if (radar_delay[i].cuenta_plot_cat48>0) {
		    l48 =    ( ((double) radar_delay[i].segmentos_ptr_cat48) / 10000.0*50.0 ) - 8.0;
		    sc48_1 = ( ((double) radar_delay[i].segmentos_cat48[radar_delay[i].segmentos_ptr_cat48]) / 10000.0*50.0 ) - 8.0;
		    sc48_2 = ( ((double) radar_delay[i].segmentos_cat48[radar_delay[i].segmentos_ptr_cat48 + 1]) / 10000.0*50.0 ) - 8.0;
		    sc48_3 = ( ((double) radar_delay[i].segmentos_cat48[radar_delay[i].segmentos_ptr_cat48 - 1]) / 10000.0*50.0 ) - 8.0;
		    if(radar_delay[i].sorted_list_cat48!=NULL) {
		        struct sorted_list *p = radar_delay[i].sorted_list_cat48; struct sorted_list *p_old = NULL;
			long count = 0; double count_percentil_99 = ((double)radar_delay[i].cuenta_plot_cat48)*99.0/100.0;
			while(p!=NULL) {
			    if ((count + p->count)>count_percentil_99) { break; }
			    count += p->count; p_old = p; p = p->next;
			}
			if (p!=NULL && p_old!=NULL) { p99_cat48 = p->segment; }
		    }
		    media = radar_delay[i].suma_retardos_cat48/radar_delay[i].cuenta_plot_cat48;
		    moda = l48 + ( (sc48_1 - sc48_3) / ( (sc48_1 - sc48_3) + (sc48_1 - sc48_2) ) ) * 0.005;
		    stdev = sqrt((radar_delay[i].suma_retardos_cuad_cat48 / radar_delay[i].cuenta_plot_cat48) - 
			pow(radar_delay[i].suma_retardos_cat48 / radar_delay[i].cuenta_plot_cat48,2));
		    update_RRD(radar_delay[i].sac, radar_delay[i].sic, 48, i,
			((long) dbp.tod_stamp) + midnight_t,
			radar_delay[i].cuenta_plot_cat48, radar_delay[i].max_retardo_cat48, radar_delay[i].min_retardo_cat48,
    			media, stdev, moda, p99_cat48);
		}
//		log_printf(LOG_NORMAL, "-----------------------------------------------------------------------------\n");
		mem_free(sac_s);
		mem_free(sic_l);
	    } // if (radar_delay[i].sac || radar_delay[i].sic)
	} // for(i=0; i<MAX_RADAR_NUMBER; i++)
//	t.tv_sec = t2.tv_sec; t.tv_usec = t2.tv_usec;
	radar_delay_clear();
//	exit(EXIT_FAILURE);
//	gettimeofday(&calcdelay2, NULL);
//	calcdelay = ((double)calcdelay2.tv_sec + calcdelay2.tv_usec/1000000.0)-((double)calcdelay1.tv_sec + calcdelay1.tv_usec/1000000.0);
//	log_printf(LOG_NORMAL, "==%03.4f==%s", calcdelay, asctime(gmtime(&t2.tv_sec)));
    } // if (forced_exit || ((dbp.tod_stamp) > step)) {

        
    
    //log_printf(LOG_NORMAL,"0) %03d %03d\n", dbp.sac, dbp.sic);
    if (dbp.available & IS_TOD) {
	int i=0;
	//diff = dbp.tod_stamp - dbp.tod;
	//if (dbp.sic==9 && dbp.cat==48)
	//log_printf(LOG_NORMAL,"%03d;%03d;%3.3f;%3.3f;%3.3f\n", dbp.sac, dbp.sic, dbp.tod_stamp, dbp.tod, diff);
	//while ( (i < MAX_RADAR_NUMBER) && 
	//    ( (dbp.sac != radar_delay[i].sac) ||
	//    (dbp.sic != radar_delay[i].sic) ) &&
	//    ( (radar_delay[i].sac != 0) ||
	//    (radar_delay[i].sic != 0) ) ) { 
	//    i++; 
	//}
        for(i=0;i<MAX_RADAR_NUMBER;i++) {
	    if ( (radar_delay[i].sac == 0) && (radar_delay[i].sic == 0) )
                break;
            if ( (radar_delay[i].sac == dbp.sac) && (radar_delay[i].sic == dbp.sic) )
                break;
        //      log_printf(LOG_ERROR, "f)CAT02] (-) %02X %02X array[%02X%02X] i(%d)\n", sac, sic, full_tod[i], full_tod[i+1],i);
        }

	if (i == MAX_RADAR_NUMBER) {
	    log_printf(LOG_ERROR, "no hay suficientes slots para radares, disponibles (%d) usados(%d)\n", MAX_RADAR_NUMBER, i); exit(EXIT_FAILURE);
	} else {
	    if ( (!radar_delay[i].sac) &&
		 (!radar_delay[i].sic) ) {
		radar_delay[i].sac = dbp.sac; radar_delay[i].sic = dbp.sic;
	    }
	}
	if (diff <= -86000) { // cuando tod esta en el dia anterior y tod_stamp en el siguiente, la resta es negativa
    	    diff += 86400;    // le sumamos un dia entero para cuadrar el calculo
        }

	//printf("retardo de sac(%d) sic(%d) demora(%3.3f)\n", dbp.sac, dbp.sic, diff);
        if (fabs(diff) >= 8.0) { // jadpascual 121219 123600
    	    //printf("retardo mayor de 8 segundos sac(%d) sic(%d) demora(%3.3f)\n", dbp.sac, dbp.sic, diff);
    	    log_printf(LOG_ERROR, "retardo mayor de 8 segundos sac(%d) sic(%d) demora(%3.3f)\n", dbp.sac, dbp.sic, diff);
    	    return;
//          exit(EXIT_FAILURE);
        }
									
	switch (dbp.cat) {
	    case CAT_01 : { 
		//if (dbp.flag_test == 1) continue;
		radar_delay[i].cuenta_plot_cat1++; radar_delay[i].suma_retardos_cat1+=diff;
		radar_delay[i].suma_retardos_cuad_cat1+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat1) radar_delay[i].max_retardo_cat1 = diff;
		if (diff < radar_delay[i].min_retardo_cat1) radar_delay[i].min_retardo_cat1 = diff;
		radar_delay[i].segmentos_cat1[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		//log_printf(LOG_NORMAL, "%3.4f;%3.4f\n", diff, ((int) floorf((diff+8.0)*10000.0/50.0))*0.005-8.0 );
		break; }
	    case CAT_02 : { 
		radar_delay[i].cuenta_plot_cat2++; radar_delay[i].suma_retardos_cat2+=diff;
		radar_delay[i].suma_retardos_cuad_cat2+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat2) radar_delay[i].max_retardo_cat2 = diff;
		if (diff < radar_delay[i].min_retardo_cat2) radar_delay[i].min_retardo_cat2 = diff;
		radar_delay[i].segmentos_cat2[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
	    case CAT_08 : { 
		radar_delay[i].cuenta_plot_cat8++;
	        radar_delay[i].suma_retardos_cat8+=diff;
		radar_delay[i].suma_retardos_cuad_cat8+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat8) radar_delay[i].max_retardo_cat8 = diff;
		if (diff < radar_delay[i].min_retardo_cat8) radar_delay[i].min_retardo_cat8 = diff;
		radar_delay[i].segmentos_cat8[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
    	    case CAT_10 : { 
		radar_delay[i].cuenta_plot_cat10++; radar_delay[i].suma_retardos_cat10+=diff;
		radar_delay[i].suma_retardos_cuad_cat10+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat10) radar_delay[i].max_retardo_cat10 = diff;
		if (diff < radar_delay[i].min_retardo_cat10) radar_delay[i].min_retardo_cat10 = diff;
		radar_delay[i].segmentos_cat10[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
	    case CAT_19 : { 
		radar_delay[i].cuenta_plot_cat19++; radar_delay[i].suma_retardos_cat19+=diff;
		radar_delay[i].suma_retardos_cuad_cat19+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat19) radar_delay[i].max_retardo_cat19 = diff;
		if (diff < radar_delay[i].min_retardo_cat19) radar_delay[i].min_retardo_cat19 = diff;
		radar_delay[i].segmentos_cat19[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
	    case CAT_20 : { 
		radar_delay[i].cuenta_plot_cat20++; radar_delay[i].suma_retardos_cat20+=diff;
		radar_delay[i].suma_retardos_cuad_cat20+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat20) radar_delay[i].max_retardo_cat20 = diff;
		if (diff < radar_delay[i].min_retardo_cat20) radar_delay[i].min_retardo_cat20 = diff;
		radar_delay[i].segmentos_cat20[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
    	    case CAT_21 : { 
		radar_delay[i].cuenta_plot_cat21++; radar_delay[i].suma_retardos_cat21+=diff;
		radar_delay[i].suma_retardos_cuad_cat21+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat21) radar_delay[i].max_retardo_cat21 = diff;
		if (diff < radar_delay[i].min_retardo_cat21) radar_delay[i].min_retardo_cat21 = diff;
		radar_delay[i].segmentos_cat21[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
	    case CAT_34 : { 
		radar_delay[i].cuenta_plot_cat34++; radar_delay[i].suma_retardos_cat34+=diff;
		radar_delay[i].suma_retardos_cuad_cat34+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat34) radar_delay[i].max_retardo_cat34 = diff;
		if (diff < radar_delay[i].min_retardo_cat34) radar_delay[i].min_retardo_cat34 = diff;
		radar_delay[i].segmentos_cat34[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
	    case CAT_48 : { 
		radar_delay[i].cuenta_plot_cat48++; radar_delay[i].suma_retardos_cat48+=diff;
		radar_delay[i].suma_retardos_cuad_cat48+=pow(diff,2);
		if (diff > radar_delay[i].max_retardo_cat48) radar_delay[i].max_retardo_cat48 = diff;
		if (diff < radar_delay[i].min_retardo_cat48) radar_delay[i].min_retardo_cat48 = diff;
		radar_delay[i].segmentos_cat48[(int) floorf((diff+8.0)*10000.0/50.0)]++;
		break; }
	    default : {
		log_printf(LOG_ERROR, "categoria desconocida sac(%d) sic(%d) cat(%d)\n", dbp.sac, dbp.sic, dbp.cat);
		exit(EXIT_FAILURE);
		break; }
	}
    }
    return;
}

void create_database(sac, sic, cat) {
char *tmp;

    tmp = mem_alloc(512);
    sprintf(tmp, "rrd_create.sh %03d_%03d_%03d %ld 2> /dev/null", sac, sic, cat, midnight_t);
    system(tmp);
    mem_free(tmp);
    return;
}

void update_RRD(int sac, int sic, int cat, int i, long timestamp, float cuenta, float max,
    float min, float media, float stdev, float moda, float p99) {
//    pid_t pid;
    char *tmp;
    
//    char *sac_s,*sic_l;
//    sac_s = ast_get_SACSIC((unsigned char *) &sac,
//	    (unsigned char *) &sic, GET_SAC_SHORT);
//    sic_l = ast_get_SACSIC((unsigned char *) &sac, 
//	(unsigned char *) &sic, GET_SIC_LONG);

    moda = (moda < -7.994) || (moda > 7.996) ? 0 : moda;
    max = (max == -10000) ? 0 : max;
    min = (min == +10000) ? 0 : min;

//    log_printf(LOG_NORMAL, "%03d_%03d_%03d %ld %3.0f %3.3f %3.3f %3.3f %3.3f %3.3f %3.3f\n",
//        sac, sic, cat, timestamp, cuenta, media, stdev, moda, max, min, p99);

//    mem_free(sac_s); mem_free(sic_l);
    
    // escribe estadísticas

	    
//    if (radar_delay[i].first_time[cat] == 0) {
//        radar_delay[i].first_time[cat] = 1;
        create_database(sac, sic, cat);
//    }
	
//    pid = fork();
//    if (pid) {
//	int status;
//	wait(&status);
//    } else { // calculo e insercion!
	tmp = mem_alloc(512);
    	sprintf(tmp, "rrd_update.sh %03d_%03d_%03d %ld %3.3f %3.3f %3.3f %3.3f %3.3f %3.3f 2> /dev/null", sac, sic, cat, 
	    timestamp, cuenta, max, min, media, stdev, p99);
	system(tmp);
	mem_free(tmp);
//    	exit(EXIT_SUCCESS);
//    }

    return;    
}
    
