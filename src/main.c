/**
 *  \file   main.c
 *  \brief  eZ430-RF2500 radio communication demo
 *  \author Antoine Fraboulet, Tanguy Risset, Dominique Tournier
 *  \date   2009
 **/

#include <msp430f2274.h>

#if defined(__GNUC__) && defined(__MSP430__)
/* This is the MSPGCC compiler */
#include <msp430.h>
#include <iomacros.h>
#elif defined(__IAR_SYSTEMS_ICC__)
/* This is the IAR compiler */
//#include <io430.h>
#endif

#include <stdio.h>
#include <limits.h>
#include <string.h>

#include "isr_compat.h"
#include "leds.h"
#include "clock.h"
#include "timer.h"
#include "button.h"
#include "uart.h"
#include "adc10.h"
#include "spi.h"
#include "cc2500.h"
#include "flash.h"
#include "watchdog.h"

#include "pt.h"

#define DBG_PRINTF printf


/* 100 Hz timer A */
#define TIMER_PERIOD_MS 10

#define PKTLEN 21
/* 0u = 0 unsigned */ 
#define MSG_BYTE_DEST 0U
#define MSG_BYTE_TYPE 1U    //type du contenu du message : température ou ACK 
#define MSG_BYTE_NODE_ID 2U    //id de la source 
#define MSG_BYTE_CONTENT 3U
#define MSG_TYPE_ID_REPLY 0x01
#define MSG_TYPE_TEMPERATURE 0x02
#define MSG_TYPE_ACK 0x03
#define MSG_TYPE_IS_ROUTER 0x05 // type de message utilisé en broadcast : la valeur correspond à l'id du routeur. 
#define MSG_TYPE_RESULTS 0x04  // tpye de message utilisé entre le routeur et le anchor, pour envoyer les résultats agglomérés


#define NODE_ID_LOCATION INFOD_START
#define NODE_ID_VALUE 1
#define BROADCAST_DEST 0x00      // valeur indiquée dans le champ "DEST" d'un message pour un broadcast.

/* 10 seconds to reply to an id request */
#define ID_INPUT_TIMEOUT_SECONDS 10
/* the same in timer ticks */
#define ID_INPUT_TIMEOUT_TICKS (ID_INPUT_TIMEOUT_SECONDS*1000/TIMER_PERIOD_MS)
static unsigned char node_id;
static unsigned char router_id; 

#define ROUTER_ID 0x01

#define NUM_TIMERS 7
static uint16_t timer[NUM_TIMERS];
#define TIMER_LED_RED_ON timer[0]
#define TIMER_LED_GREEN_ON timer[1]
#define TIMER_ANTIBOUNCING timer[2]
#define TIMER_RADIO_SEND timer[3]
#define TIMER_ID_INPUT timer[4]
#define TIMER_RADIO_FORWARD timer[5]
#define TIMER_RADIO_BROADCAST timer[6] 

static void printhex(char *buffer, unsigned int len)
{
    unsigned int i;
    for(i = 0; i < len; i++)
    {
        printf("%02X ", buffer[i]);
    }
}

static void dump_message(char *buffer)
{
    printf(" >>>>>> 0x%02X<<<<<<<<  \n", node_id); 
    printf("message received\r\n  content: ");
    printhex(buffer, PKTLEN);
    printf("\r\n  from node: 0x");
    printf("%02X\r\n", buffer[MSG_BYTE_NODE_ID]);
    printf("  to destination : %02X\r\n", buffer[MSG_BYTE_DEST]); 

    if(buffer[MSG_BYTE_TYPE] == MSG_TYPE_TEMPERATURE)
    {
        unsigned int temperature;
        char *pt = (char *) &temperature;
        pt[0] = buffer[MSG_BYTE_CONTENT + 1];
        pt[1] = buffer[MSG_BYTE_CONTENT];
        printf("  temperature: %d\r\n", temperature);
    }
    if(buffer[MSG_BYTE_TYPE] == MSG_TYPE_ACK) 
    {
        printf("             ACK \n");  
    }
}

static void prompt_node_id()
{
    printf("A node requested an id. You have %d seconds to enter an 8-bit ID.\r\n", ID_INPUT_TIMEOUT_SECONDS);
}

/* returns 1 if the id was expected and set, 0 otherwise */
static void set_node_id(unsigned char id)
{
    TIMER_ID_INPUT = UINT_MAX;
    if(flash_write_byte((unsigned char *) NODE_ID_LOCATION, id) != 0)
    {
        flash_erase_segment((unsigned int *) NODE_ID_LOCATION);
        flash_write_byte((unsigned char *) NODE_ID_LOCATION, id);
    }
    node_id = id;
    printf("this node id is now 0x%02X\r\n", id);
}

/* met à jour l'id du routeur en local. Appelé lors de la réception d'un message de broadcast.*/
static void set_router_id(unsigned char id)
{
    if(router_id != id){ 
        router_id = id; 
        printf("Changed router id : 0x%02X\r\n", router_id) ; 
    }
    else{
        printf("router id unchanged \r\n"); 
    }
}

/* Protothread contexts */

#define NUM_PT 8
static struct pt pt[NUM_PT];


/*
 * Timer
 */

void timer_tick_cb() {
    int i;
    for(i = 0; i < NUM_TIMERS; i++)
    {
        if(timer[i] != UINT_MAX) {
            timer[i]++;
        }
    }
}

int timer_reached(uint16_t timer, uint16_t count) {
    return (timer >= count);
}


/*
 * LEDs
 */

static int led_green_duration;
static int led_green_flag;

/* asynchronous */
static void led_green_blink(int duration)
{
    led_green_duration = duration;
    led_green_flag = 1;
}

static PT_THREAD(thread_led_green(struct pt *pt))
{
    PT_BEGIN(pt);

    while(1)
    {
        PT_WAIT_UNTIL(pt, led_green_flag);
        led_green_on();
        TIMER_LED_GREEN_ON = 0;
        PT_WAIT_UNTIL(pt, timer_reached(TIMER_LED_GREEN_ON,
          led_green_duration));
        led_green_off();
        led_green_flag = 0;
    }

    PT_END(pt);
}

static PT_THREAD(thread_led_red(struct pt *pt))
{
    PT_BEGIN(pt);

    while(1)
    {
        led_red_switch();
        TIMER_LED_RED_ON = 0;
        PT_WAIT_UNTIL(pt, timer_reached(TIMER_LED_RED_ON, 100));
    }

    PT_END(pt);
}


/*
 * Radio
 */

static char radio_tx_buffer[PKTLEN];
static char radio_rx_buffer[PKTLEN];
static int radio_rx_flag;

void radio_cb(uint8_t *buffer, int size, int8_t rssi)
{
    led_green_blink(10); /* 10 timer ticks = 100 ms */
    DBG_PRINTF("radio_cb :: ");
    switch (size)
    {
        case 0:
            DBG_PRINTF("msg size 0\r\n");
            break;
        case -EEMPTY:
            DBG_PRINTF("msg emptyr\r\n");
            break;
        case -ERXFLOW:
            DBG_PRINTF("msg rx overflow\r\n");
            break;
        case -ERXBADCRC:
            DBG_PRINTF("msg rx bad CRC\r\n");
            break;
        case -ETXFLOW:
            DBG_PRINTF("msg tx overflow\r\n");
            break;
        default:
            if (size > 0)
            {
                /* register next available buffer in pool */
                /* post event to application */
                DBG_PRINTF("rssi %d\r\n", rssi);

                memcpy(radio_rx_buffer, buffer, PKTLEN);
                //FIXME what if radio_rx_flag == 1 already?
                radio_rx_flag = 1;
            }
            else
            {
                /* packet error, drop */
                DBG_PRINTF("msg packet error size=%d\r\n",size);
            }
            break;
    }

    cc2500_rx_enter();
}

static void radio_send_message()
{
    cc2500_utx(radio_tx_buffer, PKTLEN);
    printf("sent: ");
    printhex(radio_tx_buffer, PKTLEN);
    putchar('\r');
    putchar('\n');
    cc2500_rx_enter();
}

static float return_average(char *buffer) 
{
    char *temp_buffer = buffer[MSG_BYTE_CONTENT]; 
    float average = 0; 
    unsigned int temperature = 0;
    char *pt = &temperature; 
    int nb_nodes = (int)temp_buffer[0]; 
    printf("nb_nodes : %d \r\n", nb_nodes); 
    int i=2; 
    for(i=2 ; i< nb_nodes*4 + 1; i=i+4)
    {
        pt[0] = temp_buffer[i+1]; 
        pt[1] = buffer[i]; 
        printf(" id %02X : %02X\r\n", temp_buffer[i-1], temperature) ; 
        average += temperature; 
    }
    average = average / nb_nodes; 

    return average; 
}

static void print_average(char *buffer)
{
    float average = return_average(buffer); 
    printf("Temperature : %f " , average) ;
}


static void handle_message(char *buffer)
{
    // si ce message est bien destiné au noeud, répondre par un ACK 
    if(buffer[MSG_BYTE_DEST] == node_id && buffer[MSG_BYTE_TYPE] == MSG_TYPE_TEMPERATURE){
        send_ack(buffer[MSG_BYTE_NODE_ID] ); 
    }
    // si ce message est bien destiné au noeud et est de type "TYPE_RESULTS" (du routeur vers le anchor uniquement)
    if(buffer[MSG_BYTE_DEST] == node_id && buffer[MSG_BYTE_TYPE] == MSG_TYPE_RESULTS){
        send_ack(buffer[MSG_BYTE_NODE_ID] ); 
        print_average(buffer); 
    }

    // si le message est un broadcast (dest=0) et que le type est "IS_ROUTER" (advertise de l'id du routeur), mettre à jour l'id du routeur en local
    if(buffer[MSG_BYTE_DEST] == BROADCAST_DEST && buffer[MSG_BYTE_TYPE] == MSG_TYPE_IS_ROUTER){
        set_router_id(buffer[MSG_BYTE_NODE_ID]); 
    }
    if (buffer[MSG_BYTE_TYPE] == MSG_TYPE_ACK) 
    {
        printf("Ce message est un ACK\n") ;
    }
    if(buffer[MSG_BYTE_DEST] != node_id) 
    {
        printf("Ce message ne m'est pas destiné.\n") ; 
    }
}

static PT_THREAD(thread_process_msg(struct pt *pt))
{
    PT_BEGIN(pt);

    while(1)
    {
        PT_WAIT_UNTIL(pt, radio_rx_flag == 1);

        dump_message(radio_rx_buffer);

        handle_message(radio_rx_buffer); 

        radio_rx_flag = 0;
    }

    PT_END(pt);
}


/*
 * UART
 */

static int uart_flag;
static uint8_t uart_data;

int uart_cb(uint8_t data)
{
    uart_flag = 1;
    uart_data = data;
    return 0;
}

/* to be called from within a protothread */
static void init_message()
{
    unsigned int i;
    for(i = 0; i < PKTLEN; i++)
    {
        radio_tx_buffer[i] = 0x00;
    }
    radio_tx_buffer[MSG_BYTE_NODE_ID] = node_id;
}

/* to be called from within a protothread */
static void send_temperature()
{
    init_message();
    radio_tx_buffer[MSG_BYTE_TYPE] = MSG_TYPE_TEMPERATURE;
    radio_tx_buffer[MSG_BYTE_DEST] = router_id  ; 
    int temperature = adc10_sample_temp();
    printf("temperature: %d, hex: ", temperature);
    printhex((char *) &temperature, 2);
    putchar('\r');
    putchar('\n');
    /* msp430 is little endian, convert temperature to network order */
    char *pt = (char *) &temperature;
    radio_tx_buffer[MSG_BYTE_CONTENT] = pt[1];
    radio_tx_buffer[MSG_BYTE_CONTENT + 1] = pt[0];
    radio_send_message();
}

/* to be called by router to advertise its id */
static void send_router_id()
{
    init_message();
    radio_tx_buffer[MSG_BYTE_TYPE] = MSG_TYPE_IS_ROUTER;
    radio_tx_buffer[MSG_BYTE_DEST] = BROADCAST_DEST; 
    printf("Advertising router id \r\n");
    radio_send_message();
}

// unused 
//
static void send_id_reply(unsigned char id)
{
    init_message();
    radio_tx_buffer[MSG_BYTE_TYPE] = MSG_TYPE_ID_REPLY;
    radio_tx_buffer[MSG_BYTE_CONTENT] = id;
    radio_send_message();
    printf("ID 0x%02X sent\r\n", id);
}

void send_ack(unsigned char dest_id) 
{
    init_message(); 
    radio_tx_buffer[MSG_BYTE_TYPE] = MSG_TYPE_ACK; 
    radio_tx_buffer[MSG_BYTE_DEST] = dest_id;
    radio_send_message(); 
    printf("Sent ACK to 0x%02X \r\n", dest_id); 
}

static PT_THREAD(thread_uart(struct pt *pt))
{
    PT_BEGIN(pt);

    while(1)
    {
        PT_WAIT_UNTIL(pt, uart_flag);

        led_green_blink(10); /* 10 timer ticks = 100 ms */

	set_node_id(NODE_ID_VALUE);
        uart_flag = 0;
    }

    PT_END(pt);
}

/*
 * Button
 */

#define ANTIBOUNCING_DURATION 10 /* 10 timer counts = 100 ms */
static int antibouncing_flag;
static int button_pressed_flag;

void button_pressed_cb()
{
    if(antibouncing_flag == 0)
    {
        button_pressed_flag = 1;
        antibouncing_flag = 1;
        TIMER_ANTIBOUNCING = 0;
        led_green_blink(200); /* 200 timer ticks = 2 seconds */
    }
}

static PT_THREAD(thread_button(struct pt *pt))
{
    PT_BEGIN(pt);

    while(1)
    {
        PT_WAIT_UNTIL(pt, button_pressed_flag == 1);

        TIMER_ID_INPUT = 0;

        /* ask locally for a node id and broadcast an id request */
        prompt_node_id();

        button_pressed_flag = 0;
    }


    PT_END(pt);
}

static PT_THREAD(thread_antibouncing(struct pt *pt))
{
    PT_BEGIN(pt);

    while(1)
      {
        PT_WAIT_UNTIL(pt, antibouncing_flag
          && timer_reached(TIMER_ANTIBOUNCING, ANTIBOUNCING_DURATION));
        antibouncing_flag = 0;
    }

    PT_END(pt);
}

#ifdef TAG
static PT_THREAD(thread_periodic_send(struct pt *pt))
{
    PT_BEGIN(pt);

    while(1)
    {
        TIMER_RADIO_SEND = 0;
        PT_WAIT_UNTIL(pt, timer_reached( TIMER_RADIO_SEND, 1000));
        send_temperature();
    }

    PT_END(pt);
}
#endif

#ifdef ROUTER 
static PT_THREAD(thread_periodic_broadcast(struct pt *pt))
{
    PT_BEGIN(pt); 
    while(1) 
    {
        TIMER_RADIO_BROADCAST = 0; 
        PT_WAIT_UNTIL(pt, timer_reached( TIMER_RADIO_BROADCAST, 10000)); 
        send_router_id(); 
    }
    PT_END(pt) ; 
}
#endif 

/*
 * main
 */

int main(void)
{
    watchdog_stop();

    TIMER_ID_INPUT = UINT_MAX;

    /* protothreads init */
    int i;
    for(i = 0; i < NUM_PT; i++)
    {
        PT_INIT(&pt[i]);
    }

    /* clock init */
    set_mcu_speed_dco_mclk_16MHz_smclk_8MHz();

    /* id init */
    set_node_id(NODE_ID_VALUE); 
    set_router_id(ROUTER_ID); 

    /* LEDs init */
    leds_init();
    led_red_on();
    led_green_flag = 0;

    /* timer init */
    timerA_init();
    timerA_register_cb(&timer_tick_cb);
    timerA_start_milliseconds(TIMER_PERIOD_MS);

    /* button init */
    button_init();
    button_register_cb(button_pressed_cb);
    antibouncing_flag = 0;
    button_pressed_flag = 0;

    /* UART init (serial link) */
    uart_init(UART_9600_SMCLK_8MHZ);
    uart_register_cb(uart_cb);
    uart_flag = 0;
    uart_data = 0;

    /* ADC10 init (temperature) */
    adc10_start();

    /* radio init */
    spi_init();
    cc2500_init();
    cc2500_rx_register_buffer(radio_tx_buffer, PKTLEN);
    cc2500_rx_register_cb(radio_cb);
    cc2500_rx_enter();
    radio_rx_flag = 0;

#ifdef ANCHOR
    printf("ANCHOR RUNNING: \r\n");
#endif
#ifdef TAG
    printf("TAG RUNNING: \r\n");
#endif
#ifdef ROUTER
    printf("ROUTER RUNNING \r\n"); 
#endif 
    printf("node id retrieved from flash: %d\r\n", node_id);
    button_enable_interrupt();
    __enable_interrupt();

    /* simple cycle scheduling */
    while(1) {
        thread_led_red(&pt[0]);
        thread_led_green(&pt[1]);
        thread_uart(&pt[2]);
        thread_antibouncing(&pt[3]);
        thread_process_msg(&pt[4]);
#ifdef TAG
        thread_periodic_send(&pt[5]);
#endif
#ifdef ROUTER 
        thread_periodic_broadcast(&pt[6]); 
#endif 
        thread_button(&pt[7]);
    }
}
