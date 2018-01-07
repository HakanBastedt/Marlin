/*
  laser.cpp - Laser control library for Arduino using 16 bit timers- Version 1
  Copyright (c) 2013 Timothy Schmidt.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "laser.h"
#include "Configuration.h"
#include "configuration_store.h"
#include "pins.h"
#include <avr/interrupt.h>
#include <Arduino.h>
#include "Marlin.h"

laser_t laser;

#ifndef PE3                   // Undef'd in fastio.h.
#define PE3 3
#endif

#define bit(x) (1 << x)
void timer3_init(int pin) 
{
  TCCR3A = 0;                 // clear control register A 
  TCCR3B = bit(WGM33);        // set mode as phase and frequency correct pwm, stop the timer

  ICR3 = F_CPU / LASER_PWM / 2;  // the counter runs backwards after TOP
  TCCR3B &= ~(bit(CS30) | bit(CS31) | bit(CS32)); // Stop timer

  TCCR3A |= bit(COM3A1);      // Connect pin5 to timer register
  DDRE |= bit(PORTE3);        // Actually output on pin 5

  OCR3A = 0;                  // Zero duty cycle = OFF
  TCCR3B |= bit(CS30);        // No prescaler, start timer

// Use timer4 to end laser pulse
/*
Prescaler  CS42  CS41  CS40  Range
   1         0     0     1   0 - 4.08 msec
   8         0     1     0   0 - 32.7 ms  <=====
  64         0     1     1   0 - 261 ms
 256         1     0     0   0 - 1046 ms
1024         1     0     1   0 - 4183 ms

6000 mm/min at 508 dpi = 0.5 ms pulse
300 mm/min at 254 dpi = 20 ms pulse
For the moment a prescaler of 8 is used which
allows up to 32.7 ms pulses with a theoretical
resolution of 0.5 µs. 

Waveform generation mode 4: CTC top in OCR4A
============================================
WGN43, WGM42, WGM41, WGM40 = 0, 1, 0, 0

TCCR4A
======
COM4A1, COM4A0 = 0,0 = Normal operation, OC4A disconnected
COM4B1, COM4B0 = 0,0 = Normal operation, OC4B disconnected
COM4C1, COM4C0 = 0,0 = Normal operation, OC4C disconnected
WGM41, WGM40 = 0,0 (See above)
TCCR4B
======
ICN4, IEC4 = 0,0 = Not applicable without input
WGM43, WGM42 = 0,1 (See above)
CS42, CS41, CS40 = 0,1,0 (See above)
CS42, CS41, CS40 = 0,0,0 = Clock stopped

TCCR4C
======
FOC4A, FOC4B, FOS4B = 0,0,0 = Not used

OCR4A
=====
16-bit value when timer overflows = generated interrupt
This is set in laser_pulse()

TIMSK4
======
OCIE4A = 1 = Generate interrupt when timer reach OCR4A

TIFR4
=====
OCF4A: When set, the interrupt will be executed. To clear, write 1 here
When reloading the timer in laser_pulse, an expired interrupt is cleared.

*/
  // Prepare laser pulse shutdown timer
  TCCR4A = 0; 
  TCCR4B = bit(WGM42);    // CTC
  TIMSK4 |= bit(OCIE4A);  // Enable interrupt on OCR4A
}

ISR(TIMER4_COMPA_vect) 
{
  OCR3A = 0;              // 0 Duty cycle

  // Stop pulse shutdown timer
  TCCR4B &= ~(bit(CS40) | bit(CS41) | bit(CS42)); // Stop timer.
}

void laser_pulse(uint32_t ulValue, unsigned long usec)
{
  OCR3A = ulValue;        // Duty cycle of pulse

  // Start timer4 to end pulse
  OCR4A = 2*usec;         // Ticks until IRQ, "2" comes from prescaler
  TCNT4 = 0;              // Count from 0
  TCCR4B |= bit(CS41);    // Start timer
  TIFR4 = bit(OCF4A);     // Clear any pending interrupt
}

void laser_init()
{
  // Initialize timers for laser intensity control 
  // ONLY laser_firing on pin 5. Can't use pin 6 for output, used by timer4.
  timer3_init(LASER_FIRING_PIN);

  #ifdef LASER_PERIPHERALS
  digitalWrite(LASER_PERIPHERALS_PIN, HIGH);  // Laser peripherals are active LOW, so preset the pin
  pinMode(LASER_PERIPHERALS_PIN, OUTPUT);

  digitalWrite(LASER_PERIPHERALS_STATUS_PIN, HIGH);  // Set the peripherals status pin to pull-up.
  pinMode(LASER_PERIPHERALS_STATUS_PIN, INPUT);
  #endif // LASER_PERIPHERALS

  // initialize state to some sane defaults
  laser.intensity = 0.0;
  laser.ppm = 0.0;
  laser.duration = 0;
  laser.status = LASER_OFF;
  laser.firing = LASER_OFF;
  laser.mode = CONTINUOUS;
  laser.last_firing = 0;
  laser.diagnostics = false;
  laser.time = 0;
  #ifdef LASER_RASTER
    laser.raster_aspect_ratio = LASER_RASTER_ASPECT_RATIO;
    laser.raster_mm_per_pulse = LASER_RASTER_MM_PER_PULSE;
    laser.raster_direction = 1;
  #endif // LASER_RASTER
  #ifdef MUVE_Z_PEEL
    laser.peel_distance = 2.0;
    laser.peel_speed = 2.0;
    laser.peel_pause = 0.0;
  #endif // MUVE_Z_PEEL
}

void laser_fire(float intensity = 100.0)
{
  return;
  laser.firing = LASER_ON;
  laser.last_firing = micros(); // microseconds of last laser firing
  if (intensity > 100.0) intensity = 100.0; // restrict intensity between 0 and 100
  if (intensity < 0) intensity = 0;
      
#define SEVEN 0
  // In the case that the laserdriver need at least a certain level "SEVEN"
  // to give anything, the intensity can be remapped to start at "SEVEN"
  // At least some CO2-drivers need it, not sure about laserdiode drivers.
#if SEVEN != 0
#define OldRange (255.0 - 0.0);
#define NewRange = (255.0 - SEVEN); 
  intensity = intensity * NewRange / OldRange + SEVEN;
#endif

#if LASER_CONTROL == 1
  OCR3A = labs((intensity / 100.0)*(F_CPU / LASER_PWM / 2));
#endif
#if LASER_CONTROL == 2
  analogWrite(LASER_INTENSITY_PIN, labs((intensity / 100.0)*(F_CPU / LASER_PWM))); // Fix
  digitalWrite(LASER_FIRING_PIN, LASER_ARM);
#endif
  
  if (laser.diagnostics) {
     SERIAL_ECHOLN("Laser fired");
  }
}
void laser_extinguish()
{
  if (laser.firing == LASER_ON) {
    laser.firing = LASER_OFF;

    OCR3A = 0; // Zero duty cycle = OFF

    laser.time += millis() - (laser.last_firing / 1000);
        if (laser.diagnostics) {
      SERIAL_ECHOLN("Laser extinguished");
    }
  }
}
void laser_set_mode(int mode){
	switch(mode){
		case 0:
		  laser.mode = CONTINUOUS;
		  return;
		case 1:
		  laser.mode = PULSED;
		  return;
		case 2:
		  laser.mode = RASTER;
		  return;
	}
}
#ifdef LASER_PERIPHERALS
bool laser_peripherals_ok(){
	return !digitalRead(LASER_PERIPHERALS_STATUS_PIN);
}
void laser_peripherals_on(){
	digitalWrite(LASER_PERIPHERALS_PIN, LOW);
	if (laser.diagnostics) {
	  SERIAL_ECHO_START;
	  SERIAL_ECHOLNPGM("Laser Peripherals Enabled");
    }
}
void laser_peripherals_off(){
	if (!digitalRead(LASER_PERIPHERALS_STATUS_PIN)) {
	  digitalWrite(LASER_PERIPHERALS_PIN, HIGH);
	  if (laser.diagnostics) {
	    SERIAL_ECHO_START;
	    SERIAL_ECHOLNPGM("Laser Peripherals Disabled");
      }
    }
}
void laser_wait_for_peripherals() {
	unsigned long timeout = millis() + LASER_PERIPHERALS_TIMEOUT;
	if (laser.diagnostics) {
	  SERIAL_ECHO_START;
	  SERIAL_ECHOLNPGM("Waiting for peripheral control board signal...");
	}
	while(!laser_peripherals_ok()) {
		if (millis() > timeout) {
			if (laser.diagnostics) {
			  SERIAL_ERROR_START;
			  SERIAL_ERRORLNPGM("Peripheral control board failed to respond");
			}
			Stop();
			break;
		}
	}
}
#endif // LASER_PERIPHERALS
