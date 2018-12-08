/*
	Copyright 2016 Benjamin Vedder	benjamin@vedder.se

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef LEDPWM_H_
#define LEDPWM_H_

// Settings
#define LEDPWM_LED_NUM		4
#define LEDPWM_CNT_TOP		50

#define LED_GREEN			0
#define LED_RED				1
#define SWITCH_LED_1             2
#define SWITCH_LED_2             3

// Functions
void ledpwm_init(void);
void ledpwm_set_intensity(unsigned int led, float intensity);
void ledpwm_led_on(int led);
void ledpwm_led_off(int led);
void ledpwm_update_pwm(void);

#endif /* LEDPWM_H_ */
