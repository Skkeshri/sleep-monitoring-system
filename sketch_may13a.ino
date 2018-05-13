////////////////////////////////////
// clock AND protoThreads configure!
// You MUST check this file!
#include "config.h"
// threading library
#include "pt_cornell_1_2_1.h"

#include "fat32.h" // sd card

// Stores the input for a given sleep
struct input_t {
    int  bpm_data[300];
    int   bpm_data_size;
    
    int  breathing_data[300];
    int   breathing_data_size;
    
    int  movement_data[300];
    int   movement_data_size;
};
typedef struct input_t input_t;

// Processed data (analyzed form of input_t)
struct results_t {
    int average_bpm;
    int average_breathing;
    int average_movement;
    int average_deriv_bpm;
    int average_deriv_breathing;
    int average_deriv_movement;
    int overall_quality;
};
typedef struct results_t results_t;

////////////////////////////////////
// graphics libraries
// SPI channel 1 connections to TFT
#include "tft_master.h"
#include "tft_gfx.h"
// need for rand function
#include <stdlib.h>
#include <stdint.h>
#include "sdcard_replacement.h"

////////////////////////////////////

////////////////////////////////////
// pullup/down macros for keypad
// PORT B
#define EnablePullDownB(bits) CNPUBCLR=bits; CNPDBSET=bits;
#define DisablePullDownB(bits) CNPDBCLR=bits;
#define EnablePullUpB(bits) CNPDBCLR=bits; CNPUBSET=bits;
#define DisablePullUpB(bits) CNPUBCLR=bits;
//PORT A
#define EnablePullDownA(bits) CNPUACLR=bits; CNPDASET=bits;
#define DisablePullDownA(bits) CNPDACLR=bits;
#define EnablePullUpA(bits) CNPDACLR=bits; CNPUASET=bits;
#define DisablePullUpA(bits) CNPUACLR=bits;
////////////////////////////////////

#define INPUT_SIZE 8
#define ARDUINO_CS BIT_4

// menu variables //////////////////
#define MENU_0_MAX 1
int menu_id = 0;
int selection_max_id = 2;
int selection_id = 0;
int sleep_history_selection = 0;

char sleep_mode_flag = 0;
unsigned long long sleep_start_time = 0;
unsigned long long time_lapsed  = 0;

char gui_update_flag = 1;
////////////////////////////////////

// debounce FSM ////////////////////
#define NO_PUSH       0
#define MAYBE_PUSH    1
#define PUSHED        2
#define WAIT_RELEASE  3

int CURRENT_STATE; 
int BUTTON;
////////////////////////////////////

// input variables /////////////////
int last_x = 0,  last_y = 0,  last_z = 0;
short chaos_x = 0, chaos_y = 0, chaos_z = 0;
int bpm = 0;
int breathing_rate = 0;

char history_names[3][40] = { "Night 1 (Good sleep)", "Night 2 (Bad sleep)", "Sleep Mode Acquisition"};
int history_name_size = 3;
////////////////////////////////////

static struct pt pt_draw, pt_userinput, pt_test;
static uint8_t input_buffer[INPUT_SIZE + 1];

/*
 * Set the button pins to inputs and enable pullups
 * 
 * Buttons must be connected to +3.3V !
 */
void init_buttons(){
    mPORTBSetPinsDigitalIn(BIT_7 | BIT_8 | BIT_9);
    EnablePullDownB(BIT_7 | BIT_8 | BIT_9);
}

/*void init_SPI2(){
    // SCK2 is pin 26 
    // SDO2 (MOSI) is in PPS output group 2, could be connected to RB5 which is pin 14
    PPSOutput(2, RPB5, SDO2);
    PPSInput(3,  SDI2, RPB13); // RBP13 --> SDI2

    // control CS
    mPORTBSetPinsDigitalOut(ARDUINO_CS);
    mPORTBSetBits(ARDUINO_CS);
    
    SpiChnOpen(SPI_CHANNEL2, SPI_OPEN_ON | SPI_OPEN_MODE8 | SPI_OPEN_MSTEN , 20);
}*/


/*
 *  Initialize the SPI peripheral in framed slave mode
 *  RPB5 is used as MOSI
 *  RPB13 is used as MISO
 *  RB3 is used as CS
 */
void init_SPIslave(){
    SPI2CON = 0;            //Clears config register
    int rData = SPI2BUF;   //Clears receive buffer
    SPI2STATCLR = 0x40;    //Clears overflow
    
    // Open SPI Channel 2 as a slave
    SpiChnOpen(2, SPI_OPEN_FSP_IN|SPI_OPEN_MODE16|SPI_OPEN_CKE_REV| SPI_OPEN_ON|SPI_OPEN_FRMEN, 80);
    
    PPSOutput(2, RPB5, SDO2);
    PPSInput(3,  SDI2, RPB13); // RBP13 --> SDI2
    //PPSInput(3, SS2, RPA3);
    mPORTASetPinsDigitalIn(BIT_3);
}

/*
 * Initialize the UART peripheral at 115200 baud
 * Other parameters: 8 bit, no parity, 1 stop bit
 */
void init_UART(){
    UARTConfigure(UART2, UART_ENABLE_PINS_TX_RX_ONLY);
    UARTSetFifoMode(UART2, UART_INTERRUPT_ON_TX_NOT_FULL | UART_INTERRUPT_ON_RX_NOT_EMPTY);
    UARTSetLineControl(UART2, UART_DATA_SIZE_8_BITS | UART_PARITY_NONE | UART_STOP_BITS_1);
    UARTSetDataRate(UART2, 40000000, 115200);
    UARTEnable(UART2, UART_ENABLE_FLAGS(UART_PERIPHERAL | UART_RX | UART_TX));
    
    PPSInput (2, U2RX, RPA1); //Assign U2RX to pin RPB11 -- Physical pin 22 on 28 PDIP
    PPSOutput(4, RPB10, U2TX); //Assign U2TX to pin RPB10 -- Physical pin 21 on 28 PDIP
}

/*
 * Grab data from the UART peripheral (for debugging)
 */
UINT32 GetDataBuffer(char *buffer, UINT32 max_size)
{
    UINT32 num_char;

    num_char = 0;

    while(num_char < max_size)
    {
        UINT8 character;

        while(!UARTReceivedDataIsAvailable(UART2))
            ;

        character = UARTGetDataByte(UART2);

        if(character == '\r')
            break;

        *buffer = character;

        buffer++;
        num_char++;
    }

    return num_char;
}


/*void read_from_glove(){
    int i;
    for (i = 0; i < INPUT_SIZE+1; i++){
        mPORTBClearBits(ARDUINO_CS); // start transaction
        // test for ready
        while (TxBufFullSPI2());
        // write to spi2
        input_buffer[i] = WriteSPI2(i);
        // test for done
        while (SPI2STATbits.SPIBUSY); // wait for end of transaction
        // CS high
        mPORTBSetBits(ARDUINO_CS); // end transaction 
    }     
}*/

/*
 * Erase a line of height 15 and length [length]
 * Then write a line over the erased area
 */
void write_TFT_line(int x, int y, int length, char* buffer){
    tft_fillRect(x, y, length, 15, ILI9340_BLACK);
    tft_setCursor(x, y); 
    tft_setTextSize(2);
    tft_setTextColor(ILI9340_WHITE);
    tft_writeString(buffer);
}

#define X_MAX 320
#define Y_MAX 240
#define POINT_RADIUS 1

/* 
 * Draw a point on the screen (uses five pixels in a cross shape for visbility)
 */
inline void draw_point(int x, int y, unsigned short color){
    tft_drawPixel(x,y,color);
    tft_drawPixel(x + POINT_RADIUS,y, color);
    tft_drawPixel(x - POINT_RADIUS,y, color);
    tft_drawPixel(x, y + POINT_RADIUS, color);
    tft_drawPixel(x, y - POINT_RADIUS, color);                    
}

static char dbgbuf[64];

/*
 * Draw a graph of arbitrary number of points and y range
 * Will graph in multiples of 300, if points less than 300 are specified, the entire graph won't be filled up
 *   If more than 300 points are specified, points are averaged evenly until 300 is reached
 */
void draw_graph( int * data, int size, unsigned short color ){
    // Draw a graph (yay)
    
    // Draw y-axis
    tft_drawFastVLine(10, 0, Y_MAX-10, ILI9340_WHITE);
    
    // Draw x-axis
    tft_drawFastHLine(10, Y_MAX-10, X_MAX-10, ILI9340_WHITE);
    
    // Find the y max and min
    int max = 0, min = 0;
    int i; 
    for (i = 0; i < size; i++){
        int point = data[i];
        if (point > max) max = point;
        if (point < min) min = point;
    }
    
    /*sprintf(dbgbuf, "MAX: %d", data[5]);
    write_TFT_line(10, 30, 200, dbgbuf);
    sprintf(dbgbuf, "MIN: %d", data[9]);
    write_TFT_line(10, 50, 200, dbgbuf);*/
    
    // Figure out what multiple of 300 we are
    int mult_300 = size/300; 
    size = mult_300 * 300;
   
    // Now condense the data array into a new array
    int new_data_index = 0;
    for (i = 0; i < size; i += mult_300){
        int j, average = 0;
        for (j = i; j < i + mult_300; j++)
            average += data[j];
        data[new_data_index++] = average / mult_300;
    }
    
    int MAX_Y = 100;
    // Separate out into a max of 50 points
    for (i = 0; i < 300; i++){
        int data_point = data[i];
        int y_point = (data_point - min) * ( (Y_MAX-MAX_Y) - 0) / (max - min) + 0;
        draw_point( 10+(i),  Y_MAX - (10 + y_point ), color);
    }
}

/*
 *  Changes the currently active menu
 *  Resets the selection_id and sets the correct selection_max_id based on the new menu
 * 
 *  Does not modify gui_update_flag!
 */
void goto_menu(char index){
    if ( index == 0 ){
        menu_id = 0;
        selection_id = 0;
        selection_max_id = 2;
    }else if ( index == 1 ){
        menu_id = 1;
        selection_id = 0;
        selection_max_id = 0;
    }else if ( index == 2 ){
        menu_id = 2;
        selection_id = 0;
        selection_max_id = 2;
    }else if ( index == 3){
        menu_id = 3;
        selection_id = 0;
        selection_max_id = history_name_size;
    }else if ( index == 4 ){
        sleep_history_selection = selection_id;
        menu_id = 4;
        selection_id = 0;
        selection_max_id = 3;
    }
    tft_fillRoundRect(0, 0, 320, 240, 1, ILI9340_BLACK); // reset screen
}

/* 
 * Called when a successful complete button press is identified
 * 
 * The button that was pressed is passed in as the [index] parameter, with the following mappings:
 *   - 0: Up Button
 *   - 1: Select/Enter Button
 *   - 2: Down Button
 */
void button_push(char index){
    if (index == 0){
        // Move selection up
        if ( selection_id < selection_max_id ) selection_id++;
    }else if (index == 1){
        // Change menu
        if (menu_id == 0){ 
            if ( selection_id == 0 ){  
                goto_menu(1); // Go to demo mode
            }else if (selection_id == 1){
                goto_menu(2); // Go to sleep mode
            }else if (selection_id == 2){
                goto_menu(3); // Go to sleep history mode
            }
        }else if (menu_id == 1){
            goto_menu(0);
        }else if (menu_id == 2){
            // TODO: Implement
            sleep_mode_flag = 0;
            goto_menu(0); //exit sleep mode
        }else if (menu_id == 3){
            // Choose previous sleeps
            if (selection_id < selection_max_id){
                goto_menu(4);
            }else{
                goto_menu(0); // Go back button
            }
        }else if (menu_id == 4){
            goto_menu(3);
        }
    }else if (index == 2){
        // Move selection down
        if ( selection_id > 0 ) selection_id--;
    }
    gui_update_flag = 1;
}

/*
 *  Takes raw data input in the form of an input_t and performs sleep analysis on it
 *  
 *  Uses average and derivative data to calculate a quality of sleep metric
 *  Returns a results_t with the findings
 */
results_t analyze_data(input_t * indata){
    results_t outdata;
    
    int i;
        
    // Calculate average BPM
    int bpm_sum = 0;
    int bpm_deriv_sum = 0;
    for (i = 0; i < indata->bpm_data_size; i++){
        bpm_sum += indata->bpm_data[i];
        if (i+1 < indata->bpm_data_size)
            bpm_deriv_sum += abs(indata->bpm_data[i+1] - indata->bpm_data[i]);
    }
    outdata.average_bpm = bpm_sum / indata->bpm_data_size;
    outdata.average_deriv_bpm = 1000*bpm_deriv_sum / (indata->bpm_data_size - 1);
    
    // Calculate average breathing rate
    int breathe_sum = 0;
    int breathe_deriv_sum = 0;
    for (i = 0; i < indata->breathing_data_size; i++){
        breathe_sum += indata->breathing_data[i];
        if (i+1 < indata->breathing_data_size)
            breathe_deriv_sum += abs(indata->breathing_data[i+1] - indata->breathing_data[i]);
    }
    outdata.average_breathing = breathe_sum / indata->breathing_data_size;
    outdata.average_deriv_breathing = (500*breathe_deriv_sum /(indata->breathing_data_size - 1));
    
    // Calculate average movement
    int move_sum = 0;
    int move_deriv_sum = 0;
    for (i = 0; i < indata->movement_data_size; i++){
        move_sum += indata->movement_data[i];
        if (i+1 < indata->movement_data_size)
            move_deriv_sum += abs(indata->movement_data[i+1] - indata->movement_data[i]);
    }
    outdata.average_movement = move_sum / indata->movement_data_size;
    outdata.average_deriv_movement = 250*move_deriv_sum / (indata->movement_data_size - 1);
    
    // Calculate total sleep quality
    float chaos_sum = outdata.average_deriv_movement + outdata.average_deriv_breathing + outdata.average_deriv_bpm;
    
    outdata.overall_quality = (int) 100 - ((chaos_sum - 600.0f) / 12.0f);
    
    // in case of anomalies
     if (outdata.overall_quality <= 0) outdata.overall_quality = 10;
     else if (outdata.overall_quality >= 100) outdata.overall_quality = 90;
   
    return outdata;
}

/*
 *  Draw thread, responsible for drawing all of the menus and showing the 
 *    currently highlighted menu options
 */
static PT_THREAD (protothread_draw(struct pt *pt)){
   PT_BEGIN(pt);
   
   static char strbuf[64];
   static input_t realtimedata;
   static j=0; // index for realtimedata arrays
   
   while(1){       
       if (!gui_update_flag){
           PT_YIELD_TIME_msec(100);
           continue;
       }
       gui_update_flag = 0;
             
       if ( menu_id == 0){
           // Home screen
           sprintf(strbuf, "Sleep Quality Meter");
           write_TFT_line(10, 10, 200, strbuf);
           
           sprintf(strbuf, "by Nick and Julia");
           write_TFT_line(10, 30, 200, strbuf);
           
           sprintf(strbuf, "[%s] Demo Mode", selection_id == 0 ? "*" : " ");
           write_TFT_line(10, 70, 200, strbuf);
           
           sprintf(strbuf, "[%s] Sleep Mode", selection_id == 1 ? "*" : " ");
           write_TFT_line(10, 90, 200, strbuf);
           
           sprintf(strbuf, "[%s] See My History", selection_id == 2 ? "*" : " ");
           write_TFT_line(10, 110, 200, strbuf);
          
       }else if ( menu_id == 1){
           // Demo Mode
           sprintf(strbuf, "Demo Mode");
           write_TFT_line(10, 10, 200, strbuf);
           
           sprintf(strbuf, "Press any key to exit");
           write_TFT_line(10, 30, 200, strbuf);
           
           sprintf(strbuf, "Heartrate: %d", bpm);
           write_TFT_line(10, 50, 200, strbuf);
           
           sprintf(strbuf, "X Chaos: %d", chaos_x);
           write_TFT_line(10, 70, 200, strbuf);
           
           sprintf(strbuf, "Y Chaos: %d", chaos_y);
           write_TFT_line(10, 90, 200, strbuf);
           
           sprintf(strbuf, "Z Chaos: %d", chaos_z);
           write_TFT_line(10, 110, 200, strbuf);
           
           sprintf(strbuf, "Breathing Rate: %d", breathing_rate);
           write_TFT_line(10, 130, 250, strbuf);
       }else if ( menu_id == 2 ){
           sprintf(strbuf, "[%s] Go Back", selection_id == 0 ? "*" : " ");
           write_TFT_line(10, 10, 200, strbuf);
           // Sleep mode screen
           //Have a timer to show duration of sleep --> set flag to keep track
           if(sleep_mode_flag == 0){
               sleep_mode_flag = 1;
                time_lapsed = 0;
                j=0;
                sleep_start_time = PT_GET_TIME();//how do i mark the end, when we tab out?
                int n;
                for(n = 0; n<300; n++){
                    realtimedata.bpm_data[n] = 0;
                    realtimedata.breathing_data[n]= 0;
                    realtimedata.movement_data[n] = 0;
                }
                realtimedata.bpm_data_size = 0;
                realtimedata.breathing_data_size = 0;
                realtimedata.movement_data_size = 0;
                
           }
           else{
               time_lapsed = (PT_GET_TIME()-sleep_start_time)/1000; //seconds
               sprintf(strbuf, "Time Elapsed : %d", time_lapsed);
               write_TFT_line(10, 10 + 20*1, 250, strbuf);
               //add data to struct, how should cycle through this?
               if (j>=300){
                   j = 0;
               }else{
                   realtimedata.bpm_data[j] = bpm;
                   realtimedata.breathing_data[j]= 1 + breathing_rate;
                   realtimedata.movement_data[j] = (abs(chaos_x) + abs(chaos_y) + abs(chaos_z))/100;
                   j++;
               }
               realtimedata.bpm_data_size = j;
               realtimedata.breathing_data_size = j;
               realtimedata.movement_data_size = j;
               inputs[2] = realtimedata; 
            
           }
                  
           // 300 length array and make another history one--> create new array         
           
       }else if ( menu_id == 3 ){
           int i;
           for (i = 0; i < history_name_size; i++){
               sprintf(strbuf, "[%s] %s", selection_id == i ? "*" : " ", history_names[i]);
               write_TFT_line(10, 10 + 20*i, 200, strbuf);
           }
           sprintf(strbuf, "[%s] Go Back", selection_id == i ? "*" : " ");
           write_TFT_line(10, 10 + 20*i, 200, strbuf);
       }else if ( menu_id == 4 ){
           //inputs[2] = realtimedata;
           tft_fillRoundRect(0, 0, 320, 240, 1, ILI9340_BLACK); // reset screen
           if ( selection_id == 0 ){
               results_t results = analyze_data(&inputs[sleep_history_selection]);
               
               // Summary screen
               sprintf(strbuf, "Sleep Summary");
               write_TFT_line(10, 10, 200, strbuf);
               
               sprintf(strbuf, "Avg. Movement: %d", results.average_movement);
               write_TFT_line(10, 50, 200, strbuf);
               sprintf(strbuf, "Avg. Heartrate: %d", results.average_bpm);
               write_TFT_line(10, 70, 200, strbuf);
               sprintf(strbuf, "Avg. Breathing: %d", results.average_breathing);
               write_TFT_line(10, 90, 200, strbuf);
               
               sprintf(strbuf, "Movement Chaos: %d", results.average_deriv_movement);
               write_TFT_line(10, 130, 200, strbuf);
               sprintf(strbuf, "Heartrate Chaos: %d", results.average_deriv_bpm);
               write_TFT_line(10, 150, 200, strbuf);
               sprintf(strbuf, "Breathing Chaos: %d", results.average_deriv_breathing);
               write_TFT_line(10, 170, 200, strbuf);
               
               sprintf(strbuf, "Sleep Quality: %d %s", results.overall_quality, "%");
               write_TFT_line(10, 200, 200, strbuf);
           }else{
                /*int data[1000];
                int i, value = 0;
                for (i = 0; i < 1000; i++){
                    data[i] = value;
                    value += (rand() & 0x1) ;
                }*/
               //inputs[2] = realtimedata;
               if ( selection_id == 1){
                   sprintf(strbuf, "Heart Rate");
                   write_TFT_line(15, 15, 50, strbuf);
                   draw_graph(inputs[sleep_history_selection].bpm_data, inputs[sleep_history_selection].bpm_data_size, ILI9340_RED);
               }else if (selection_id == 2){
                   sprintf(strbuf, "Movement");
                   write_TFT_line(15, 15, 50, strbuf);
                   draw_graph(inputs[sleep_history_selection].movement_data, inputs[sleep_history_selection].movement_data_size, ILI9340_CYAN);
               }else if (selection_id == 3){
                   sprintf(strbuf, "Breathing Rate");
                   write_TFT_line(15, 15, 50, strbuf);
                   draw_graph(inputs[sleep_history_selection].breathing_data, inputs[sleep_history_selection]. breathing_data_size, ILI9340_GREEN);
               }
           }
       }
       
     /*read_from_glove();
     
     uint8_t i;
     for (i = 1; i < INPUT_SIZE + 1; i++){
         sprintf(strbuf, "%d: %d", i, input_buffer[i]);
         write_TFT_line(10, 2 + i*12, 50, strbuf);
     }*/
    
     PT_YIELD_TIME_msec(100);
        // NEVER exit while
    } // END WHILE(1)
  PT_END(pt);
}

/*
 * Polls each the buttons and runs a debounce FSM on each to determine if they have been pushed
 * 
 * Calls button_pushed() with the corresponding button inputs
 */
static PT_THREAD (protothread_userinput(struct pt *pt)){
   PT_BEGIN(pt);
   
   static char strbuf[64];
   
   static int counter = 0;
     
   while(1){
        int i = -1;
        int input = mPORTBReadBits(BIT_7 | BIT_8 | BIT_9);
        if (input & BIT_7) i = 0;
        else if (input & BIT_8) i = 1;
        else if (input & BIT_9) i = 2;
        
        // DEBOUNCE FSM
        //  a button must be the only held down button for two scans
        //  (approx. 60ms) in order to register as a button press
        if(CURRENT_STATE == NO_PUSH){
            if(i != -1){
                CURRENT_STATE = MAYBE_PUSH;
                BUTTON = i;
            }
        }
        else if(CURRENT_STATE == MAYBE_PUSH){
            if(i != BUTTON){
                CURRENT_STATE = NO_PUSH;
            }
            else if(i == BUTTON){
                CURRENT_STATE = PUSHED;
            }
        }
        else if(CURRENT_STATE == PUSHED){
            CURRENT_STATE = WAIT_RELEASE;
            button_push(BUTTON);          
        }
        else if(CURRENT_STATE == WAIT_RELEASE){
            if(i != BUTTON){
                CURRENT_STATE = NO_PUSH;
            }
        }
        
        PT_YIELD_TIME_msec(25);
   }
   
   PT_END(pt);
}

static char strbuf2[64];

/*
 *  Called when data is successfully received over SPI
 * 
 *  Parses the id and correctly assigns the data to the correct input variable
 */
void SPI_rec(uint16_t id, uint16_t rx){
    if (menu_id == 2) gui_update_flag = 1;
    switch (id){
        case 0: 
            chaos_x = abs(rx) - last_x; 
            last_x = abs(rx);
            break;
        case 1: 
            chaos_y = abs(rx) - last_y;
            last_y = abs(rx);
            break;
        case 2: 
            chaos_z = abs(rx) - last_z;
            last_z = abs(rx);
            break;
        case 3: 
            if ( rx > 60 && rx < 110)
                bpm = rx; 
            break;
        case 4: 
            if ( rx > 8 && rx < 16 )
                breathing_rate = rx; break;
        case 5: 
            if ( menu_id == 1 || menu_id == 2)
                gui_update_flag = 1; 
            break;
    }
}

/*
 *  debugging and spi/uart recieve thread
 */
static PT_THREAD (protothread_test(struct pt *pt)){
    PT_BEGIN(pt);
    
    init_SPIslave();
       
   static char strbuf[64];
   static uint16_t uart_buffer[8];
   static int a = 0;
     
   while(1){
       //init_UART();
       
       PT_YIELD_TIME_msec(10);
       init_SPIslave();
       
       if (menu_id == 1 || menu_id == 2){
            sprintf(strbuf, "%d", a++);
            write_TFT_line(10, 200, 50, strbuf);
       }
       
       
       //GetDataBuffer(uart_buffer, 8);
       if (mPORTAReadBits(BIT_3)) {
           continue;
       }
       
       int j;
       for (j = 0; j < 2; j++){
           uart_buffer[j] = SpiChnGetC(2);
           //sprintf(strbuf, "hi: %d %d", a++, uart_buffer[j]>>1);
           //write_TFT_line(10, 10 + 20*j, 300, strbuf);
       }
       
       SPI_rec(uart_buffer[0]>>1,  uart_buffer[1]>>1);
       
   }
   
   PT_END(pt);
}

// === Main  ======================================================
void main(void) {
  // === config threads ==========
  // turns OFF UART support and debugger pin, unless defines are set
  PT_setup();

  // === setup system wide interrupts  ========
  INTEnableSystemMultiVectoredInt();
  
  // init the threads
  PT_INIT(&pt_draw);
  PT_INIT(&pt_userinput);
  PT_INIT(&pt_test);

  // init the display
  // NOTE that this init assumes SPI channel 1 connections
  tft_init_hw();
  tft_begin();
  tft_fillScreen(ILI9340_BLACK);
  //240x320 vertical display
  tft_setRotation(1); // Use tft_setRotation(1) for 320x240
  
  //init_SPI2();
  sdcardrepl_init();
  init_buttons();
  
  /*setupSPI();
  int error = SD_init();
  
  char strbuf[64];
  sprintf(strbuf, "SD response: %d", error);
  write_TFT_line(10, 110, 200, strbuf);*/
  
  // round-robin scheduler for threads
  while (1){
      PT_SCHEDULE(protothread_draw(&pt_draw));
      PT_SCHEDULE(protothread_userinput(&pt_userinput));
      PT_SCHEDULE(protothread_test(&pt_test));
    }
  } // main

// === end  ======================================================

