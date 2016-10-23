/*------------------------------------------------------------------
 *  in4073.c -- test QR engines and sensors
 *
 *  reads ae[0-3] uart rx queue
 *  (q,w,e,r increment, a,s,d,f decrement)
 *
 *  prints timestamp, ae[0-3], sensors to uart tx queue
 *
 *  I. Protonotarios
 *  Embedded Software Lab
 *
 *  June 2016
 *------------------------------------------------------------------
 */
#include "in4073.h"
#include "protocol/protocol.h"
#include "states.h"
#include "logging.c"

#define BAT_THRESHOLD  1050

#define MIN_RPM 204800
#define MAX_RPM 768000

#define int_to_fixed_point(a) (((int16_t)a)<<8)
#define divide_fixed_points(a,b) (int)((((int32_t)a<<8)+(b/2))/b)
#define fixed_point_to_int(a) (int)(a>>8)



//restore the number received from pc side, due to protocol coding
char restore_num(char num)
{
	char n;
	if((num&0x40)==0x40)
	{
		n=num|0x80;
	}
	else
	{
		n=num;
	}

	return n;
}


//in this function calculate the values for the ae[] array makis
void calculate_rpm(int Z, int L, int M, int N)
{
	int ae1[4],i;
	//if there is lift force calculate ae[] array values
	if(Z>0)
	{		
		//calculate the square of each motor rpm, according to the assignment.pdf equations
		ae1[0]=(2*M-N+Z)/4 + MIN_RPM;
		ae1[1]=(2*L+N+Z)/4 - L + MIN_RPM;
		ae1[2]=(2*M-N+Z)/4 - M + MIN_RPM;
		ae1[3]=(2*L+N+Z)/4 + MIN_RPM;

		//minimum rpm
		for(i=0;i<4;i++)
		{	
			if(ae1[i]<MIN_RPM)
			{
				ae1[i]=MIN_RPM;
			}
		}
		//maximum rpm
		for(i=0;i<4;i++)
		{	
			if(ae1[i]>MAX_RPM)
			{
				ae1[i]=MAX_RPM;
			}
		}

		//get the final motor values, instead of square root calculation just scale down	
		ae[0]=ae1[0]>>10;
		ae[1]=ae1[1]>>10;
		ae[2]=ae1[2]>>10;
		ae[3]=ae1[3]>>10;
	}
	//if there is no lift force everything should be shut down
	else if(Z<=0)
	{
		ae[0]=0;
		ae[1]=0;
		ae[2]=0;
		ae[3]=0;
	}
	//update motors
	run_filters_and_control();
}

//calibration mode state makis
void calibration_mode()
{
	cur_mode=CALIBRATION_MODE;
	logging_enabled = true;

	//indicate that you are in calibration mode
	nrf_gpio_pin_write(RED,1);
	nrf_gpio_pin_write(YELLOW,1);
	nrf_gpio_pin_write(GREEN,0);

	//counter for calibration samples
	int clb;
	clb=0;

	//take 256 samples 
	while(clb<256)
	{
		if (check_sensor_int_flag())
		{
			get_dmp_data();
			clear_sensor_int_flag();
			clb++;
			p_off=p_off+sp;
			q_off=q_off+sq;
			r_off=r_off+sr;
			phi_off=phi_off+phi;
			theta_off=theta_off+theta;
			printf("sp=%d, sq=%d, sr=%d,phi=%d,theta=%d\n",sp,sq,sr,phi,theta);
		}
		if(msg==true)
		{
			process_input();
		}	
	}

	//calculate the offset
	p_off=p_off>>8;
	q_off=q_off>>8;
	r_off=r_off>>8;
	phi_off=phi_off>>8;
	theta_off=theta_off>>8;	

	//fix a bug, don't care to check connection getting to safe mode anyway
	time_latest_packet_us=get_time_us();
	//print offsets of calibrated values
	printf("sp_off=%ld, sq_off=%ld, sr_off=%ld,phi_off=%ld,theta_off=%ld\n",p_off,q_off,r_off,phi_off,theta_off);

	
	//print once in safe mode
	safe_print=true;

	//get back to safe mode
	statefunc=safe_mode;
}


void full_control_mode()
{
	
	cur_mode=FULL_CONTROL_MODE;
	logging_enabled = true;

	//indicate that you are in full control mode
	nrf_gpio_pin_write(RED,1);
	nrf_gpio_pin_write(YELLOW,0);
	nrf_gpio_pin_write(GREEN,0);

	//while no new message has been received, enter this control loop
	while(msg==false && connection==true)
	{
		check_connection();
		if (check_sensor_int_flag())
		{
			get_dmp_data();				
			clear_sensor_int_flag();
			calculate_rpm(lift_force, roll_moment + (roll_moment-(phi-phi_off)*4)*p1_ctrl-(sp-p_off)*4*p2_ctrl, pitch_moment + (pitch_moment-(theta-theta_off)*4)*p1_ctrl+(sq-q_off)*4*p2_ctrl,yaw_moment - (yaw_moment-(sr-r_off)*4)*p_ctrl);		
		}
	
		if (check_timer_flag())
		{			
			if (counter++%20 == 0)
			{		
		 		nrf_gpio_pin_toggle(BLUE);
				battery_check();
				printf("Full Controlled Mode, Battery: %d, p:%d, p1:%d, p2:%d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,p_ctrl,p1_ctrl,p2_ctrl,ae[0],ae[1],ae[2],ae[3]);
			}
			clear_timer_flag();
		}
	}

	//read the new message
	process_input();

	//act according to the received message
	switch (pc_packet.mode)	
	{			
		case PANIC_MODE:
			statefunc=panic_mode;
			break;
		case FULL_CONTROL_MODE:
			cur_lift=pc_packet.lift;
			cur_pitch=pc_packet.pitch;
			cur_roll=pc_packet.roll;
			cur_yaw=pc_packet.yaw;
			if(pc_packet.p_adjust==1)
			{
				p_ctrl=p_ctrl+1;
				if(p_ctrl>=127)
				{
					p_ctrl=127;
				}
			}
			if(pc_packet.p_adjust==2)
			{
				p_ctrl=p_ctrl-1;
				if(p_ctrl<=0)
				{
					p_ctrl=0;
				}
			}
			if(pc_packet.p_adjust==0x4)
			{
				p1_ctrl=p1_ctrl+1;
				if(p1_ctrl>=127)
				{
					p1_ctrl=127;
				}
			}
			if(pc_packet.p_adjust==0x8)
			{
				p1_ctrl=p1_ctrl-1;
				if(p1_ctrl<=0)
				{
					p1_ctrl=0;
				}
			}
			if(pc_packet.p_adjust==0x10)
			{
				p2_ctrl=p2_ctrl+1;
				if(p2_ctrl>=127)
				{
					p2_ctrl=127;
				}
			}
			if(pc_packet.p_adjust==0x20)
			{
				p2_ctrl=p2_ctrl-1;
				if(p2_ctrl<=0)
				{
					p2_ctrl=0;
				}
			}
			break;
		default:
			printf("Invalid mode transition!!!\n");
			break;
	}


	//if there is a new command do the calculations
	if(old_lift!=cur_lift || old_pitch!=cur_pitch || old_roll!=cur_roll || old_yaw!=cur_yaw)	
	{

		//map pc-side info to forces and moments
		lift_force=cur_lift*9676;

		if(cur_roll>63)
		{
			roll_moment=(int)(((cur_roll-128)>>3)*1270);
		}
		else
		{
			roll_moment=(int)((cur_roll>>3)*1270);
		}
		
		if(cur_pitch>63)
		{
			pitch_moment=(int)(((cur_pitch-128)>>3)*1270);
		}
		else
		{
			pitch_moment=(int)((cur_pitch>>3)*1270);
		}
		
		if(cur_yaw>63)
		{
			yaw_moment=(int)(((cur_yaw-128)>>3)*1270);
		}
		else
		{
			yaw_moment=(int)((cur_yaw>>3)*1270);
		}
		
		//calculate the motor values
		//calculate_rpm(lift_force,roll_moment,pitch_moment,yaw_moment);

		//make sure to enter again when something new happens
		old_lift=cur_lift;
		old_roll=cur_roll;
		old_pitch=cur_pitch;
		old_yaw=cur_yaw;		

		
		//print statement
		printf("Full Controlled Mode, Battery: %d, p:%d, p1:%d, p2:%d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,p_ctrl,p1_ctrl,p2_ctrl,ae[0],ae[1],ae[2],ae[3]);
	}	

		
	
}



void yaw_control_mode()
{
	
	cur_mode=YAW_CONTROLLED_MODE;
	logging_enabled = true;
	
	//indicate that you are in yaw control mode
	nrf_gpio_pin_write(RED,0);
	nrf_gpio_pin_write(YELLOW,1);
	nrf_gpio_pin_write(GREEN,0);


	while(msg==false && connection==true)
	{
		check_connection();
		if (check_sensor_int_flag())
		{
			get_dmp_data();				
			clear_sensor_int_flag();
			calculate_rpm(lift_force,roll_moment,pitch_moment,yaw_moment - (yaw_moment-sr*4)*p_ctrl);		
		}
	
		if (check_timer_flag()) 
		{			
			if (counter++%20 == 0)
			{
				 nrf_gpio_pin_toggle(BLUE);

				battery_check();

				printf("Yaw Controlled Mode, Battery: %d, p:%d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,p_ctrl,ae[0],ae[1],ae[2],ae[3]);
			}
			clear_timer_flag();
		}
	}

	//read the message
	process_input();
	
	//act according to the message just read
	switch (pc_packet.mode)	
	{
		case PANIC_MODE:
			statefunc=panic_mode;
			break;
		case YAW_CONTROLLED_MODE:
			cur_lift=pc_packet.lift;
			cur_pitch=pc_packet.pitch;
			cur_roll=pc_packet.roll;
			cur_yaw=pc_packet.yaw;
			if(pc_packet.p_adjust==1)
			{
				p_ctrl=p_ctrl+1;
				if(p_ctrl>=127)
				{
					p_ctrl=127;
				}
			}
			if(pc_packet.p_adjust==2)
			{
				p_ctrl=p_ctrl-1;
				if(p_ctrl<=0)
				{
					p_ctrl=0;
				}
			}
			break;
		default:
			printf("Invalid mode transition!!!\n");
			break;
	}


	//if there is a new command do the calculations
	if(old_lift!=cur_lift || old_pitch!=cur_pitch || old_roll!=cur_roll || old_yaw!=cur_yaw)	
	{

		//map pc-side info to forces and moments
		lift_force=cur_lift*9676;

		if(cur_roll>63)
		{
			roll_moment=(int)(((cur_roll-128)>>3)*1270);
		}
		else
		{
			roll_moment=(int)((cur_roll>>3)*1270);
		}
		
		if(cur_pitch>63)
		{
			pitch_moment=(int)(((cur_pitch-128)>>3)*1270);
		}
		else
		{
			pitch_moment=(int)((cur_pitch>>3)*1270);
		}
		
		if(cur_yaw>63)
		{
			yaw_moment=(int)(((cur_yaw-128)>>3)*1270);
		}
		else
		{
			yaw_moment=(int)((cur_yaw>>3)*1270);
		}
		
		//calculate the motor values
		//calculate_rpm(lift_force,roll_moment,pitch_moment,yaw_moment);

		//make sure to enter again when something new happens
		old_lift=cur_lift;
		old_roll=cur_roll;
		old_pitch=cur_pitch;
		old_yaw=cur_yaw;		
		//print statement
		printf("Yaw Controlled Mode, Battery: %d, p:%d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,p_ctrl,ae[0],ae[1],ae[2],ae[3]);
	}	
}


//manual mode state makis
void manual_mode()
{
	cur_mode=MANUAL_MODE;
	logging_enabled = true;

	//indicate that you are in manual mode
	nrf_gpio_pin_write(RED,1);
	nrf_gpio_pin_write(YELLOW,0);
	nrf_gpio_pin_write(GREEN,1);


	//while there is no message received wait here, check your connection, check your battery and toggle the blue led	
	while(msg==false && connection==true)
	{
		check_connection();
		if (check_timer_flag()) {			
			if (counter++%20 == 0) 
			{
				nrf_gpio_pin_toggle(BLUE);
				battery_check();
			}
			clear_timer_flag();
		}
	}

	//read the new messages to come
	process_input();

	//act according to the incoming message
	switch (pc_packet.mode)	
	{
		case SAFE_MODE:
			statefunc=safe_mode;
		case PANIC_MODE:
			statefunc=panic_mode;
			break;
		case MANUAL_MODE:
			cur_lift=pc_packet.lift;
			cur_pitch=pc_packet.pitch;
			cur_roll=pc_packet.roll;
			cur_yaw=pc_packet.yaw;
			break;
		default:
			printf("Invalid mode transition!!!\n");
			break;
	}

	//if there is a new command do the calculations
	if(old_lift!=cur_lift || old_pitch!=cur_pitch || old_roll!=cur_roll || old_yaw!=cur_yaw)	
	{

		//map pc-side info to forces and moments
		lift_force=cur_lift*9676;

		if(cur_roll>63)
		{
			roll_moment=(int)((cur_roll-128)*1270);
		}
		else
		{
			roll_moment=(int)(cur_roll*1270);
		}
		
		if(cur_pitch>63)
		{
			pitch_moment=(int)((cur_pitch-128)*1270);
		}
		else
		{
			pitch_moment=(int)(cur_pitch*1270);
		}
		
		if(cur_yaw>63)
		{
			yaw_moment=(int)((cur_yaw-128)*12700);
		}
		else
		{
			yaw_moment=(int)(cur_yaw*12700);
		}
		
		//calculate the motor values
		calculate_rpm(lift_force,roll_moment,pitch_moment,yaw_moment);

		//make sure to enter again when something new happens
		old_lift=cur_lift;
		old_roll=cur_roll;
		old_pitch=cur_pitch;
		old_yaw=cur_yaw;
		
		//print statement
		printf("Manual Mode, Battery: %d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,ae[0],ae[1],ae[2],ae[3]);
	}	



	
}

//panic mode state makis
void panic_mode()
{
	cur_mode=PANIC_MODE;
	logging_enabled = true;
	print_pc_enabled = true;

	//indicate that you are in panic mode
	nrf_gpio_pin_write(RED,0);
	nrf_gpio_pin_write(YELLOW,0);
	nrf_gpio_pin_write(GREEN,1);


	//toggle blue led
	if (check_timer_flag())
	{			
		if (counter++%20 == 0)
		{
			nrf_gpio_pin_toggle(BLUE);
		}
	}
	

	//decrease motor speed untill motors are off
	if(ae[0]>175 || ae[1]>175 || ae[2]>175 || ae[3]>175) {
		ae[0]-=10;
		ae[1]-=10;
		ae[2]-=10;
		ae[3]-=10;
		run_filters_and_control();
		//delay for next round
		nrf_delay_ms(200);
		printf("Panic Mode, ae[0]=%d, ae[1]=%d, ae[2]=%d, ae[3]=%d\n",ae[0],ae[1],ae[2],ae[3]);
		clear_timer_flag();
	}

	//enters safe mode
	if(ae[0]<=175 || ae[1]<=175 || ae[2]<=175 || ae[3]<=175)
	{
		statefunc=safe_mode;
	}

	//zero down some values
	cur_lift=0;
	cur_pitch=0;
	cur_roll=0;
	cur_yaw=0;
	old_lift=0;
	old_pitch=0;
	old_roll=0;
	old_yaw=0;
	lift_force=0;
	roll_moment=0;
	yaw_moment=0;
	pitch_moment=0;

	//fixes a bug, doesn't care to check connection going to safe mode anyway
	time_latest_packet_us=get_time_us();

	//flag to print only once in safe mode
	safe_print=true;

}

//safe mode state makis 
void safe_mode()
{
	cur_mode=SAFE_MODE;	
	logging_enabled = false;

	//indicate that you are in safe mode	
	if(connection==false)
	{
		nrf_gpio_pin_write(RED,0);
		nrf_gpio_pin_write(YELLOW,0);
		nrf_gpio_pin_write(GREEN,0);
	}
	else
	{
		nrf_gpio_pin_write(RED,0);
		nrf_gpio_pin_write(YELLOW,1);
		nrf_gpio_pin_write(GREEN,1);
	}
	
	//motors are shut down
	ae[0]=0;
	ae[1]=0;
	ae[2]=0;
	ae[3]=0;
	run_filters_and_control();


	//while no message is received wait here, check your connection, check your battery, toggle the blue led
	while(msg==false && connection==true)
	{
		check_connection();
		if (check_timer_flag())
		{			
			if (counter++%20 == 0)
			{
				nrf_gpio_pin_toggle(BLUE);
			}

			battery_check();

			//print only once in safe mode
			if(safe_print)
			{
				printf("Safe Mode, Battery: %d\n",bat_volt);
				safe_print=false;		
			}

			clear_timer_flag();
		}
	}
	
	//if there is battery and the connection is ok read the messages
	if(battery==true && connection==true)
	{
		process_input();
		switch (pc_packet.mode)
		{
			case MANUAL_MODE:
				if(pc_packet.lift==0 && pc_packet.pitch==0 && pc_packet.roll==0 && pc_packet.yaw==0)
				{
					printf("Manual Mode, Battery: %d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,ae[0],ae[1],ae[2],ae[3]);
					statefunc=manual_mode;
				} 
				else
				{
					printf("offsets do not match 0!\n");
					nrf_delay_ms(10);
				}
				break;
			case CALIBRATION_MODE:
				p_off=0;
				q_off=0;
				r_off=0;
				statefunc=calibration_mode;
				break;
			case YAW_CONTROLLED_MODE:
				if(pc_packet.lift==0 && pc_packet.pitch==0 && pc_packet.roll==0 && pc_packet.yaw==0)
				{
					printf("Yaw Controlled Mode, Battery: %d, p:%d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,p_ctrl,ae[0],ae[1],ae[2],ae[3]);
					statefunc=yaw_control_mode;
				} else {
					printf("offsets do not match 0!\n");
					nrf_delay_ms(10);
				}
				break;
			case FULL_CONTROL_MODE:
				if(pc_packet.lift==0 && pc_packet.pitch==0 && pc_packet.roll==0 && pc_packet.yaw==0)
				{
					printf("Full Controlled Mode, Battery: %d, p:%d, p1:%d, p2:%d, ae[0]: %d, ae[1]: %d, ae[2]: %d, ae[3]: %d\n",bat_volt,p_ctrl,p1_ctrl,p2_ctrl,ae[0],ae[1],ae[2],ae[3]);
					statefunc=full_control_mode;
				} else {
					printf("offsets do not match 0!\n");
					nrf_delay_ms(10);
				}
				break;
			case DUMP_FLASH_MODE:
				read_flight_data();
				break;
			case SHUTDOWN_MODE:
				printf("\n\t Goodbye \n\n");
				nrf_delay_ms(100);
				NVIC_SystemReset();
				break;
			default:
				break;
		}
	}
}

/*jmi*/
char get_checksum(packet p) {	
	char checksum = (p.header^p.mode^p.p_adjust^p.lift^p.pitch^p.roll^p.yaw) >> 1;
	return checksum;
}

/*jmi*/
void process_input() 
{
	//temporary package before checksum validation
	packet tp; 
	char c;
	if(rx_queue.count>0)
	{
		/*skip through all input untill header is found*/
		while ( (c = dequeue(&rx_queue) ) != HEADER_VALUE) {
		}
		tp.header = c;
		tp.mode = dequeue(&rx_queue);		
		tp.p_adjust = dequeue(&rx_queue);
		tp.lift = dequeue(&rx_queue);
		tp.pitch = dequeue(&rx_queue);
		tp.roll = dequeue(&rx_queue);
		tp.yaw = dequeue(&rx_queue);
		tp.checksum = dequeue(&rx_queue);
		//if checksum is correct,copy whole packet into global packet	
		if (tp.checksum == (get_checksum(tp) & 0x7F)) {
			pc_packet.mode = tp.mode;
			pc_packet.p_adjust = tp.p_adjust;
			pc_packet.lift = tp.lift;
			pc_packet.pitch = tp.pitch;
			pc_packet.roll = tp.roll;
			pc_packet.yaw = tp.yaw;
			pc_packet.checksum = tp.checksum;
			time_latest_packet_us = get_time_us();
			msg=false;
		}

		/*flush rest of queue*/
		while (rx_queue.count >= 1) {
			dequeue(&rx_queue);
		}
	}
}


void initialize()
{
	//message flag initialization
	msg=false;
	
	//drone modules initialization
	uart_init();
	gpio_init();
	timers_init();
	adc_init();
	twi_init();
	imu_init(true, 100);	
	baro_init();
	spi_flash_init();
	//ble_init();
	demo_done = false;
	adc_request_sample();

	//initialise the pc_packet struct to safe values, just in case
	pc_packet.mode = SAFE_MODE;
	pc_packet.lift = 0;
	pc_packet.pitch = 0;
	pc_packet.roll = 0;
	pc_packet.yaw = 0;	
	
	//initialise rest of the variables to safe values, just in case
	cur_mode=SAFE_MODE;
	cur_lift=0;
	cur_pitch=0;
	cur_roll=0;
	cur_yaw=0;	
	old_lift=0;
	old_pitch=0;
	old_roll=0;
	old_yaw=0;
	lift_force=0;
	roll_moment=0;
	pitch_moment=0;
	yaw_moment=0;
	ae[0]=0;
	ae[1]=0;
	ae[2]=0;
	ae[3]=0;
	battery=true;
	connection=true;
	safe_print=true;
	p_ctrl=40;
	p1_ctrl=12;
	p2_ctrl=30;
	p_off=0;
	q_off=0;
	r_off=0;
	theta_off=0;
	phi_off=0;

	statefunc= safe_mode;

	decoupled_from_drone = true;
	counter = 0;

	erase_flight_data(); 
	logging_enabled = false;
}

//if nothing received for over 500ms approximately go to panic mode and exit
void check_connection()
{
	current_time_us=get_time_us();
	uint32_t diff = current_time_us - time_latest_packet_us;
	if(diff > 500000)
	{	
		connection=false;
		statefunc=panic_mode;
	}
}


//jmi
void battery_check(){
	//for testing purpose, the bat is not connected.
	if(decoupled_from_drone) {
		return; 
	}
	adc_request_sample();
	//printf("Battery:%d\n",bat_volt);
	if (bat_volt < 1050)
	{
		printf("LOW BATTERY WARNING!!!!\n");
		if(bat_volt < 1000)
		{
			printf("bat voltage %d below threshold %d\n",bat_volt,BAT_THRESHOLD);
			battery=false;
			statefunc=panic_mode;
		}
	}
}

/*------------------------------------------------------------------
 * main -- do the test
 * edited by jmi
 *------------------------------------------------------------------
 */
int main(void)
{
	//initialize the drone
	initialize();


	while (!demo_done)
	{		
		//get to the state
		(*statefunc)();

	}	
	
	printf("\n\t Goodbye \n\n");
	nrf_delay_ms(100);

	NVIC_SystemReset();
}
