 /*      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "ZProbe.h"

#include "Kernel.h"
#include "BaseSolution.h"
#include "Config.h"
#include "Robot.h"
#include "StepperMotor.h"
#include "StreamOutputPool.h"
#include "Gcode.h"
#include "Conveyor.h"
#include "Stepper.h"
#include "checksumm.h"
#include "ConfigValue.h"
#include "SlowTicker.h"
#include "Planner.h"
#include "SerialMessage.h"
#include "PublicDataRequest.h"
#include "EndstopsPublicAccess.h"
#include "PublicData.h"

#include <tuple>
#include <algorithm>
#include <vector>

#define zprobe_checksum          CHECKSUM("zprobe")
#define enable_checksum          CHECKSUM("enable")
#define probe_pin_checksum       CHECKSUM("probe_pin")
#define debounce_count_checksum  CHECKSUM("debounce_count")
#define slow_feedrate_checksum   CHECKSUM("slow_feedrate")
#define fast_feedrate_checksum   CHECKSUM("fast_feedrate")
#define probe_radius_checksum    CHECKSUM("probe_radius")
#define probe_height_checksum    CHECKSUM("probe_height")


// from endstop section
#define delta_homing_checksum    CHECKSUM("delta_homing")

#define X_AXIS 0
#define Y_AXIS 1
#define Z_AXIS 2

#define STEPPER THEKERNEL->robot->actuators
#define STEPS_PER_MM(a) (STEPPER[a]->get_steps_per_mm())
#define Z_STEPS_PER_MM STEPS_PER_MM(Z_AXIS)

#define verbose false
#define verbose1 true
#define abs(a) ((a<0) ? -a : a)

static bool abs_compare(float a, float b)
{
    return (abs(a) < abs(b));
};

void ZProbe::on_module_loaded()
{
    // if the module is disabled -> do nothing
    if(!THEKERNEL->config->value( zprobe_checksum, enable_checksum )->by_default(false)->as_bool()) {
        // as this module is not needed free up the resource
        delete this;
        return;
    }
    this->running = false;

    // load settings
    this->on_config_reload(this);
    // register event-handlers
    register_for_event(ON_GCODE_RECEIVED);
    register_for_event(ON_IDLE);
    THEKERNEL->slow_ticker->attach( THEKERNEL->stepper->get_acceleration_ticks_per_second() , this, &ZProbe::acceleration_tick );
}

void ZProbe::on_config_reload(void *argument)
{
    this->pin.from_string( THEKERNEL->config->value(zprobe_checksum, probe_pin_checksum)->by_default("nc" )->as_string())->as_input();
    this->debounce_count = THEKERNEL->config->value(zprobe_checksum, debounce_count_checksum)->by_default(0  )->as_number();

    // see what type of arm solution we need to use
    this->is_delta =  THEKERNEL->config->value(delta_homing_checksum)->by_default(false)->as_bool();
    if(this->is_delta) {
        // default is probably wrong
        this->probe_radius =  THEKERNEL->config->value(zprobe_checksum, probe_radius_checksum)->by_default(100.0F)->as_number();
    }

    this->probe_height =  THEKERNEL->config->value(zprobe_checksum, probe_height_checksum)->by_default(5.0F)->as_number();
    this->slow_feedrate = THEKERNEL->config->value(zprobe_checksum, slow_feedrate_checksum)->by_default(5)->as_number(); // feedrate in mm/sec
    this->fast_feedrate = THEKERNEL->config->value(zprobe_checksum, fast_feedrate_checksum)->by_default(100)->as_number(); // feedrate in mm/sec
}

bool ZProbe::wait_for_probe(int steps[3])
{
    unsigned int debounce = 0;
    while(true) {
        THEKERNEL->call_event(ON_IDLE);
        // if no stepper is moving, moves are finished and there was no touch
        if( !STEPPER[X_AXIS]->is_moving() && !STEPPER[Y_AXIS]->is_moving() && !STEPPER[Z_AXIS]->is_moving() ) {
            return false;
        }

        // if the touchprobe is active...
        if( this->pin.get() ) {
            //...increase debounce counter...
            if( debounce < debounce_count) {
                // ...but only if the counter hasn't reached the max. value
                debounce++;
            } else {
                // ...otherwise stop the steppers, return its remaining steps
                for( int i = X_AXIS; i <= Z_AXIS; i++ ) {
                    steps[i] = 0;
                    if ( STEPPER[i]->is_moving() ) {
                        steps[i] =  STEPPER[i]->get_stepped();
                        STEPPER[i]->move(0, 0);
                    }
                }
                return true;
            }
        } else {
            // The probe was not hit yet, reset debounce counter
            debounce = 0;
        }
    }
}

void ZProbe::on_idle(void *argument)
{
}

// single probe and report amount moved
bool ZProbe::run_probe(int& steps, bool fast)
{
    // Enable the motors
    THEKERNEL->stepper->turn_enable_pins_on();
    this->current_feedrate = (fast ? this->fast_feedrate : this->slow_feedrate) * Z_STEPS_PER_MM; // steps/sec

    // move Z down
    STEPPER[Z_AXIS]->set_speed(0); // will be increased by acceleration tick
    STEPPER[Z_AXIS]->move(true, 1000 * Z_STEPS_PER_MM); // always probes down, no more than 1000mm TODO should be 2*maxz
    if(this->is_delta) {
        // for delta need to move all three actuators
        STEPPER[X_AXIS]->set_speed(0);
        STEPPER[X_AXIS]->move(true, 1000 * STEPS_PER_MM(X_AXIS));
        STEPPER[Y_AXIS]->set_speed(0);
        STEPPER[Y_AXIS]->move(true, 1000 * STEPS_PER_MM(Y_AXIS));
    }

    this->running = true;

    int s[3];
    bool r = wait_for_probe(s);
    steps= s[Z_AXIS]; // only need z
    this->running = false;
    return r;
}

bool ZProbe::return_probe(int steps)
{
    // move probe back to where it was
    this->current_feedrate = this->fast_feedrate * Z_STEPS_PER_MM; // feedrate in steps/sec
    bool dir= steps < 0;
    steps= abs(steps);

    STEPPER[Z_AXIS]->set_speed(0); // will be increased by acceleration tick
    STEPPER[Z_AXIS]->move(dir, steps);
    if(this->is_delta) {
        STEPPER[X_AXIS]->set_speed(0);
        STEPPER[X_AXIS]->move(dir, steps);
        STEPPER[Y_AXIS]->set_speed(0);
        STEPPER[Y_AXIS]->move(dir, steps);
    }

    this->running = true;
    while(STEPPER[X_AXIS]->is_moving() || STEPPER[Y_AXIS]->is_moving() || STEPPER[Z_AXIS]->is_moving()) {
        // wait for it to complete
        THEKERNEL->call_event(ON_IDLE);
    }

    this->running = false;

    return true;
}

// calculate the X and Y positions for the three towers given the radius from the center
static std::tuple<float, float, float, float, float, float, float, float, float, float, float, float> getCoordinates(float radius)
{
    float px = 0.8660254F * radius;             // ~sin(60)
    float py = 0.5F * radius;                   // cos(60)
    float t1x = -px,          t1y = -py;        // X Tower
    float t2x = px,           t2y = -py;        // Y Tower
    float t3x = 0.0F,         t3y = radius;     // Z Tower
    float t4x = px,           t4y = py;         // Opposite X Tower
    float t5x = -px,          t5y = py;         // Opposite Y Tower
    float t6x = 0.0F,         t6y = -radius;    // Opposite Z Tower
    return std::make_tuple(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y);
}

bool ZProbe::probe_delta_tower(int& steps, float x, float y)
{
    int s;
    // move to tower
    coordinated_move(x, y, NAN, this->fast_feedrate);
    if(!run_probe(s)) return false;

    // return to original Z
    return_probe(s);
    steps= s;

    return true;
}

/* Run a calibration routine for a delta
    1. Home
    2. probe for z bed
    3. probe initial tower positions
    4. set initial trims such that trims will be minimal negative values
    5. home, probe three towers again
    6. calculate trim offset and apply to all trims
    7. repeat 5, 6 until it converges on a solution
*/
bool ZProbe::calibrate_delta_endstops(Gcode *gcode, bool keep)
{
	if(verbose)gcode->stream->printf("Calibrate_delta_tower_endstops called\n");
    float target= 0.03F;
    if(gcode->has_letter('I')) target= gcode->get_value('I'); // override default target
    if(gcode->has_letter('J')) this->probe_radius= gcode->get_value('J'); // override default probe radius

    
    if(gcode->has_letter('K')) keep= true; // keep current settings

    if(verbose) gcode->stream->printf("Calibrating Endstops: target %fmm, radius %fmm\n", target, this->probe_radius);

    // get probe points
    float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y;
    std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);
	
    float trimx= 0.0F, trimy= 0.0F, trimz= 0.0F;
    if(!keep) {
        // zero trim values
        if(!set_trim(0, 0, 0, gcode->stream)) return false;

    }else{
        // get current trim, and continue from that
        if (get_trim(trimx, trimy, trimz)) {
            if(verbose) gcode->stream->printf("Current Trim X: %f, Y: %f, Z: %f\r\n", trimx, trimy, trimz);

        } else {
            if(verbose) gcode->stream->printf("Could not get current trim, are endstops enabled?\n");
            return false;
        }
    }

    // home
    home();

    // find bed, run at fast rate
    int s;
    if(!run_probe(s, true)) return false;

    float bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed
    if(verbose) gcode->stream->printf("Bed ht is %f mm\n", bedht);
	
    // move to start position
    home();
    coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
	
	
	
    // get initial probes
    // probe the base of the X tower
    if(!probe_delta_tower(s, t1x, t1y)) return false;
    float t1z= s / Z_STEPS_PER_MM;
    if(verbose) gcode->stream->printf("0: T1 Z:%1.5f Steps:%d\n", t1z, s);

    // probe the base of the Y tower
    if(!probe_delta_tower(s, t2x, t2y)) return false;
    float t2z= s / Z_STEPS_PER_MM;
    if(verbose) gcode->stream->printf("0: T2 Z:%1.5f Steps:%d\n", t2z, s);

    // probe the base of the Z tower
    if(!probe_delta_tower(s, t3x, t3y)) return false;
    float t3z= s / Z_STEPS_PER_MM;
    if(verbose) gcode->stream->printf("0: T3 Z:%1.5f Steps:%d\n", t3z, s);
  
    float trimscale= 1.1261F; // empirically determined

    auto mm= std::minmax({t1z, t2z, t3z});
    if((mm.second-mm.first) <= target) {
        if(verbose) gcode->stream->printf("trim already set within required parameters: delta %f\n", mm.second-mm.first);
        return true;
    }

    // set trims to worst case so we always have a negative trim
    trimx += (mm.first-t1z)*trimscale;
    trimy += (mm.first-t2z)*trimscale;
    trimz += (mm.first-t3z)*trimscale;

    for (int i = 1; i <= 30; ++i) {
        // set trim
        if(!set_trim(trimx, trimy, trimz, gcode->stream)) return false;

        // home and move probe to start position just above the bed
        home();
        coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed

        // probe the base of the X tower
        if(!probe_delta_tower(s, t1x, t1y)) return false;
        t1z= s / Z_STEPS_PER_MM;
        if(verbose) gcode->stream->printf("%d: T1 Z:%1.4f Steps:%d\n", i, t1z, s);

        // probe the base of the Y tower
        if(!probe_delta_tower(s, t2x, t2y)) return false;
        t2z= s / Z_STEPS_PER_MM;
        if(verbose) gcode->stream->printf("%d: T2 Z:%1.4f Steps:%d\n", i, t2z, s);

        // probe the base of the Z tower
        if(!probe_delta_tower(s, t3x, t3y)) return false;
        t3z= s / Z_STEPS_PER_MM;
        if(verbose) gcode->stream->printf("%d: T3 Z:%1.4f Steps:%d\n", i, t3z, s);

        mm= std::minmax({t1z, t2z, t3z});
        if((mm.second-mm.first) <= target) {
            gcode->stream->printf("trim set to within required parameters: delta %f\n", mm.second-mm.first);
            break;
        }

        // set new trim values based on min difference
        trimx += (mm.first-t1z)*trimscale;
        trimy += (mm.first-t2z)*trimscale;
        trimz += (mm.first-t3z)*trimscale;

        // flush the output
        THEKERNEL->call_event(ON_IDLE);
    }

    if((mm.second-mm.first) > target) {
        gcode->stream->printf("WARNING: trim did not resolve to within required parameters: delta %f\n", mm.second-mm.first);
    }

    return true;
}

/*  probe edges to get outer positions, then probe center
    modify the delta radius until center and X converge
*/
bool ZProbe::calibrate_delta_radius(Gcode *gcode)
{
	float target= 0.03F;
    if(gcode->has_letter('I')) target= gcode->get_value('I'); // override default target
    if(gcode->has_letter('J')) this->probe_radius= gcode->get_value('J'); // override default probe radius

    if(verbose) gcode->stream->printf("Calibrating delta radius: target %1.5f, radius %1.5f\n", target, this->probe_radius);

    // get probe points
    float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y;
    std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);

    home();
    // find bed, then move to a point 5mm above it
    int s;
    if(!run_probe(s, true)) return false;
    float bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed
    if(verbose) gcode->stream->printf("Bed ht is %f mm\n", bedht);

    home();
    coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed

    // probe center to get reference point at this Z height
    int dc;
    if(!probe_delta_tower(dc, 0, 0)) return false;
    if(verbose) gcode->stream->printf("CT Z:%1.5f C:%d\n", dc / Z_STEPS_PER_MM, dc);
    float cmm= dc / Z_STEPS_PER_MM;

    // get current delta radius
    float delta_radius= 0.0F;
    BaseSolution::arm_options_t options;
    if(THEKERNEL->robot->arm_solution->get_optional(options)) {
        delta_radius= options['R'];
    }
    if(delta_radius == 0.0F) {
        gcode->stream->printf("This appears to not be a delta arm solution\n");
        return false;
    }
    options.clear();

    float drinc= 2.5F; // approx
    for (int i = 1; i <= 20; ++i) {
        // probe t1, t2, t3 and get average, but use coordinated moves, probing center won't change
        int dx, dy, dz,dax,day,daz;
        if(!probe_delta_tower(dx, t1x, t1y)) return false;
        if(verbose) gcode->stream->printf("DR%d: T1 %1.5f %1.5f Z:%1.5f Steps:%d\n", i,t1x,t1y, dx / Z_STEPS_PER_MM, dx);
        if(verbose){if(!probe_delta_tower(daz,t6x,t6y)) return false;
        if(verbose) gcode->stream->printf("DR%d: T6 %1.5f %1.5f Z:%1.5f Steps:%d\n", i,t6x,t6y, daz / Z_STEPS_PER_MM, daz);};
        if(!probe_delta_tower(dy, t2x, t2y)) return false;
        if(verbose) gcode->stream->printf("DR%d: T2 %1.5f %1.5f Z:%1.5f Steps:%d\n", i,t2x,t2y, dy / Z_STEPS_PER_MM, dy);
        if(verbose){if(!probe_delta_tower(dax,t4x,t4y)) return false;
        if(verbose) gcode->stream->printf("DR%d: T4 %1.5f %1.5f Z:%1.5f Steps:%d\n", i,t4x,t4y, dax / Z_STEPS_PER_MM, dax);};
        if(!probe_delta_tower(dz, t3x, t3y)) return false;
        if(verbose) gcode->stream->printf("DR%d: T3 %1.5f %1.5f Z:%1.5f Steps:%d\n", i,t3x,t3y, dz / Z_STEPS_PER_MM, dz);
		if(verbose){if(!probe_delta_tower(day,t5x,t5y)) return false;
        if(verbose) gcode->stream->printf("DR%d: T5 %1.5f %1.5f Z:%1.5f Steps:%d\n", i,t6x,t1y, day / Z_STEPS_PER_MM, day);};
        

        // now look at the difference and reduce it by adjusting delta radius
        float m= ((dx+dy+dz)/3.0F) / Z_STEPS_PER_MM;
        float d= cmm-m;
        gcode->stream->printf("%d: Tower Z-ave:%1.4f Off by: %1.5f\n", i, m, d);
       if(verbose){ float tm= ((dx+dy+dz+dax+day+daz)/6.0F) / Z_STEPS_PER_MM;
        float td= cmm-tm;
        gcode->stream->printf("%d: 6 Point Z-ave:%1.4f Off by: %1.5f\n", i, tm,td);
		};
       if(abs(d) <= target) break; // resolution of success

        // increase delta radius to adjust for low center
        // decrease delta radius to adjust for high center
        delta_radius += (d*drinc);

        // set the new delta radius
        options['R']= delta_radius;
        THEKERNEL->robot->arm_solution->set_optional(options);
        gcode->stream->printf("Setting delta radius to: %1.4f\n", delta_radius);

        home();
        coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // needs to be a relative coordinated move

        // flush the output
        THEKERNEL->call_event(ON_IDLE);
    }
    home();
    
    return true;
}
bool ZProbe::assess_bed(Gcode *gcode)
{
	float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y; 
    std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);
	BaseSolution::arm_options_t options;
	THEKERNEL->robot->arm_solution->get_optional(options);
	gcode->stream->printf("A%1.5f \t B%1.5f \t C%1.5f \t X%1.5f \t Y%1.5f \t Z%1.5f \t R%1.5f \t L%1.5f \n", options['A'],options['B'],options['C'],options['D'],options['E'],options['F'],options['R'],options['L']);
	int dx, dy, dz,dax,day,daz,s;
	float temp1,temp2;
	home();
    if(!run_probe(s, true)) return false;
	float bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed
	gcode->stream->printf("C \t 0 \t 0 \t %f Steps:%d\n", bedht,s);
	home();
	coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true);	
	int dc;
	if(!probe_delta_tower(dc, 0, 0)) return false;
	gcode->stream->printf("CT\t %1.5f \n", dc / Z_STEPS_PER_MM);
    if(!probe_delta_tower(dx, t1x, t1y)) return false;
     gcode->stream->printf(" A\t%1.5f\t\t%1.5f\t%1.5f\n",t1x,t1y,  dx / Z_STEPS_PER_MM);
    if(!probe_delta_tower(daz,t6x,t6y)) return false;
     gcode->stream->printf("-G\t%1.5f\t\t%1.5f\t%1.5f\n",t6x,t6y, daz / Z_STEPS_PER_MM);
    if(!probe_delta_tower(dy, t2x, t2y)) return false;
     gcode->stream->printf(" B\t%1.5f\t\t%1.5f\t%1.5f\n",t2x,t2y, dy / Z_STEPS_PER_MM);
    if(!probe_delta_tower(dax,t4x,t4y)) return false;
     gcode->stream->printf("-A\t%1.5f\t\t%1.5f\t%1.5f\n",t4x,t4y, dax / Z_STEPS_PER_MM);
    if(!probe_delta_tower(dz, t3x, t3y)) return false;
     gcode->stream->printf(" G\t%1.5f\t\t%1.5f\t%1.5f\n",t3x,t3y, dz / Z_STEPS_PER_MM);
    if(!probe_delta_tower(day,t5x,t5y)) return false;
     gcode->stream->printf("-B\t%1.5f\t\t%1.5f\t%1.5f\n",t5x,t5y, day / Z_STEPS_PER_MM);
	temp1 = t1x /2;
	temp2 = t1y /2;
    if(!probe_delta_tower(dx, temp1,temp2)) return false;
    gcode->stream->printf(" A/2\t%1.5f\t\t%1.5f\t%1.5f\n",temp1,temp2,  dx / Z_STEPS_PER_MM);
	temp1 = t6x /2;
	temp2 = t6y /2;
    if(!probe_delta_tower(daz, temp1,temp2)) return false;
    gcode->stream->printf("-G/2\t%1.5f\t\t%1.5f\t%1.5f\n",t6x/2,t6y/2, daz / Z_STEPS_PER_MM);
	temp1 = t2x /2;
	temp2 = t2y /2;
    if(!probe_delta_tower(dy, temp1,temp2)) return false;
    gcode->stream->printf(" B/2\t%1.5f\t\t%1.5f\t%1.5f\n",t2x/2,t2y/2, dy / Z_STEPS_PER_MM);
	temp1 = t4x /2;
	temp2 = t4y /2;
    if(!probe_delta_tower(dax, temp1,temp2)) return false;
    gcode->stream->printf("-A/2\t%1.5f\t\t%1.5f\t%1.5f\n",t4x/2,t4y/2, dax / Z_STEPS_PER_MM);
	temp1 = t3x /2;
	temp2 = t3y /2;
    if(!probe_delta_tower(dz, temp1,temp2)) return false;
    gcode->stream->printf(" G/2\t%1.5f\t\t%1.5f\t%1.5f\n",t3x/2,t3y/2, dz / Z_STEPS_PER_MM);
	temp1 = t5x /2;
	temp2 = t5y /2;
    if(!probe_delta_tower(day, temp1,temp2)) return false;
    gcode->stream->printf("-B/2\t%1.5f\t\t%1.5f\t%1.5f\n",t5x/2,t5y/2, day / Z_STEPS_PER_MM);
	float cartesian[3];
	float actuator[3];
	cartesian[0]= 0; cartesian[1]= 0; cartesian[2]= bedht+ (dx / Z_STEPS_PER_MM);
	THEKERNEL->robot->arm_solution->cartesian_to_actuator(cartesian,actuator);
	gcode->stream->printf("[[%1.5f,%1.5f,%1.5f]\n",actuator[0],actuator[1],actuator[2]);
	cartesian[0]= t1x; cartesian[1]= t1y; cartesian[2]= bedht+ (dx / Z_STEPS_PER_MM);
	THEKERNEL->robot->arm_solution->cartesian_to_actuator(cartesian,actuator);
	gcode->stream->printf(",[%1.5f,%1.5f,%1.5f]\n",actuator[0],actuator[1],actuator[2]);
	cartesian[0]= t6x; cartesian[1]= t6y; cartesian[2]= bedht+ (daz / Z_STEPS_PER_MM);
	THEKERNEL->robot->arm_solution->cartesian_to_actuator(cartesian,actuator);
	gcode->stream->printf(",[%1.5f,%1.5f,%1.5f]\n",actuator[0],actuator[1],actuator[2]);
	cartesian[0]= t2x; cartesian[1]= t2y; cartesian[2]= bedht+ (dy / Z_STEPS_PER_MM);
	THEKERNEL->robot->arm_solution->cartesian_to_actuator(cartesian,actuator);
	gcode->stream->printf(",[%1.5f,%1.5f,%1.5f]\n",actuator[0],actuator[1],actuator[2]);
	cartesian[0]= t4x; cartesian[1]= t4y; cartesian[2]= bedht+ (dax / Z_STEPS_PER_MM);
	THEKERNEL->robot->arm_solution->cartesian_to_actuator(cartesian,actuator);
	gcode->stream->printf(",[%1.5f,%1.5f,%1.5f]\n",actuator[0],actuator[1],actuator[2]);
	cartesian[0]= t3x; cartesian[1]= t3y; cartesian[2]= bedht+ (dz / Z_STEPS_PER_MM);
	THEKERNEL->robot->arm_solution->cartesian_to_actuator(cartesian,actuator);
	gcode->stream->printf(",[%1.5f,%1.5f,%1.5f]\n",actuator[0],actuator[1],actuator[2]);
	cartesian[0]= t5x; cartesian[1]= t5y; cartesian[2]= bedht+ (day / Z_STEPS_PER_MM);
	THEKERNEL->robot->arm_solution->cartesian_to_actuator(cartesian,actuator);
	gcode->stream->printf(",[%1.5f,%1.5f,%1.5f]\n",actuator[0],actuator[1],actuator[2]);	
	
	 return true;
}
//A full geometry calibration routine
bool ZProbe::calibrate_delta_tower_geometry(Gcode *gcode)
{
	if(verbose) gcode->stream->printf("Performing Complete Calibration called\n");
	float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y; 
    std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);
    float target= 0.03F;
	if(gcode->has_letter('I')) target= gcode->get_value('I'); // override default target
    
    int alpha,beta,gamma;
	float temp1,temp2;
	int itemp;
    bool  alpha_bad,beta_bad,gamma_bad;
    alpha_bad=true;
    beta_bad=true;
    gamma_bad=true;
    int blame_tower=-1;
	//place holder for endstop's keep 
	bool keep_endstops=false;
	if(gcode->has_letter('K')) keep_endstops= true; // keep current settings
	for (int i = 1; i <= 20; ++i) {
		if(!calibrate_delta_endstops(gcode,keep_endstops)) return false;
        if(!calibrate_delta_radius(gcode)) return false;
		keep_endstops=true;
		home();
		// find bed, then move to a point 5mm above it    
		int s;    if(!run_probe(s, true)) return false;    
		float bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
		home();
		if(verbose) gcode->stream->printf("Complete Calibration iteration: %i\n",i);
		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
		
		// probe t1, t2, t3 and get average, but use coordinated moves, probing center won't change
        int dx, dy, dz,dax,day,daz;
        if(!probe_delta_tower(dx, t1x, t1y)) return false;
        if(verbose)gcode->stream->printf("G%d:\t A \tZ:%1.5f \tSteps:%d\n", i, dx / Z_STEPS_PER_MM, dx);
        if(!probe_delta_tower(daz,t6x,t6y)) return false;
        if(verbose)gcode->stream->printf("G%d:\t!G \tZ:%1.5f \tSteps:%d\n", i, daz / Z_STEPS_PER_MM, daz);
        if(!probe_delta_tower(dy, t2x, t2y)) return false;
        if(verbose)gcode->stream->printf("G%d:\t B \tZ:%1.5f \tSteps:%d\n", i, dy / Z_STEPS_PER_MM, dy);
        if(!probe_delta_tower(dax,t4x,t4y)) return false;
        if(verbose)gcode->stream->printf("G%d:\t!A \tZ:%1.5f \tSteps:%d\n", i, dax / Z_STEPS_PER_MM, dax);
        if(!probe_delta_tower(dz, t3x, t3y)) return false;
        if(verbose)gcode->stream->printf("G%d:\t G \tZ:%1.5f \tSteps:%d\n", i, dz / Z_STEPS_PER_MM, dz);
        if(!probe_delta_tower(day,t5x,t5y)) return false;
        if(verbose)gcode->stream->printf("G%d:\t!B \tZ:%1.5f \tSteps:%d\n", i, day / Z_STEPS_PER_MM, day);
		
		// report Anti-tower findings for Potential Tower error.
		//Tower Radius reporting.
		temp1= dx-dax;
		temp1= abs(temp1);
		temp1= temp1/Z_STEPS_PER_MM;
        if( temp1 > target ) gcode->stream->printf("Marlin Method:Alpha Tower Radius: Bad Difference:%1.5f \n",temp1);
		if( temp1 <= target) gcode->stream->printf("Marlin Method:Alpha Tower Radius: Good Difference:%1.5f \n",temp1);
		temp1= dy-day;
		temp1= abs(temp1);
		temp1= temp1/Z_STEPS_PER_MM;
		if( temp1 > target ) gcode->stream->printf("Marlin Method:Beta Tower Radius: Bad Difference:%1.5f \n",temp1);
		if( temp1 <= target) gcode->stream->printf("Marlin Method:Beta Tower Radius: Good Difference:%1.5f \n",temp1);
        temp1= dz-daz;
		temp1= abs(temp1);
		temp1= temp1/Z_STEPS_PER_MM;
		if( temp1 > target ) gcode->stream->printf("Marlin Method:Gamma Tower Radius: Bad Difference:%1.5f \n",temp1);
		if( temp1 <= target) gcode->stream->printf("Marlin Method:Gamma Tower Radius: Good Difference:%1.5f \n",temp1);
		
		//Tower Angle Reporting
        temp1 = dx-day;//X tower
		temp2 = dx-daz;
		temp1= abs(temp1);
		temp1= temp1/Z_STEPS_PER_MM;
		temp2= abs(temp2);
		temp2= temp2/Z_STEPS_PER_MM;
		if(temp1 > target || temp2 > target ) gcode->stream->printf("Marlin Method:Alpha Angle: Bad  Left:%1.5f Right:%1.5f \n",temp1,temp2);
		if(temp1 <= target && temp2 <= target ) gcode->stream->printf("Marlin Method:Alpha Angle: Good  Left:%1.5f Right:%1.5f \n",temp1,temp2);
		temp1 = dy-daz;//Y Tower
		temp2 = dy-dax;
		temp1= abs(temp1);
		temp1= temp1/Z_STEPS_PER_MM;
		temp2= abs(temp2);
		temp2= temp2/Z_STEPS_PER_MM;
		if(temp1 > target || temp2 > target ) gcode->stream->printf("Marlin Method:Beta Angle: Bad  Left:%1.5f Right:%1.5f \n",temp1,temp2);
		if(temp1 <= target && temp2 <= target ) gcode->stream->printf("Marlin Method:Beta Angle: Good  Left:%1.5f Right:%1.5f \n",temp1,temp2);
		temp1 = dz-dax;// Z Tower
		temp2 = dz-day;
		temp1= abs(temp1);
		temp1= temp1/Z_STEPS_PER_MM;
		temp2= abs(temp2);
		temp2= temp2/Z_STEPS_PER_MM;
		if(temp1 > target || temp2 > target ) gcode->stream->printf("Marlin Method:Gamma Angle: Bad  Left:%1.5f Right:%1.5f \n",temp1,temp2);
		if(temp1 <= target && temp2 <= target ) gcode->stream->printf("Marlin Method:Gamma Angle: Good  Left:%1.5f Right:%1.5f \n",temp1,temp2);
        //get difference of tower and anti-tower positions
        alpha= (dx - dax);
		gcode->stream->printf("Binky's Method: Alpha and -Alpha difference: %1.2f \n",alpha/ Z_STEPS_PER_MM);
        beta = (dy - day);
		gcode->stream->printf("Binky's Method:Beta and -Beta difference: %1.2f\n",beta/ Z_STEPS_PER_MM);
        gamma= (dz - daz);
		gcode->stream->printf("Binky's Method:Gamma and -Gamma difference: %1.2f\n",gamma/ Z_STEPS_PER_MM);
        //reset tower flags
		alpha_bad=true;
		beta_bad=true;
		gamma_bad=true;
        //decide which tower is worst
		
        auto mm1 = minmax({alpha,beta,gamma},abs_compare);
		itemp=abs(alpha);
		temp1=itemp / Z_STEPS_PER_MM;
        if(temp1 < target) alpha_bad = false;
		gcode->stream->printf("temp %1.5f",temp1);
		itemp=abs(beta);
		temp1=itemp / Z_STEPS_PER_MM;
        if(temp1 < target) beta_bad  = false;
		gcode->stream->printf("temp %1.5f",temp1);
		itemp=abs(gamma);
		temp1=itemp / Z_STEPS_PER_MM;
        if(temp1 < target) gamma_bad = false;
		//decide which tower to blame
        if(alpha==mm1.second) blame_tower=1;
		if( beta==mm1.second) blame_tower=2;
		if(gamma==mm1.second) blame_tower=3;
		//if(	 alpha_bad &&  beta_bad &&  gamma_bad) blame_tower = 0; //Possible they are all off, already picked worst tower
        if( !alpha_bad &&  beta_bad &&  gamma_bad) blame_tower = 1;
        if(  alpha_bad && !beta_bad &&  gamma_bad) blame_tower = 2;
        if(  alpha_bad &&  beta_bad && !gamma_bad) blame_tower = 3;
        if(  alpha_bad && !beta_bad && !gamma_bad) blame_tower = 1;
        if( !alpha_bad &&  beta_bad && !gamma_bad) blame_tower = 2;
        if( !alpha_bad && !beta_bad &&  gamma_bad) blame_tower = 3;
		if(	!alpha_bad && !beta_bad && !gamma_bad){ blame_tower = 4;gcode->stream->printf("\n"); break;};
		gcode->stream->printf("Marlin: Blaming tower: %d\n",blame_tower);
        if(blame_tower==-1) return false;
		if(blame_tower==4) { i=20; gcode->stream->printf("Break failed,forced exit");};
        if(!fix_delta_tower_radius(gcode,blame_tower)) return false;
		//in the case of all towers being off, skip fixing the tower position
		//this is until the it becomes more consistant
        if(!fix_delta_tower_position(gcode,blame_tower)) return false;
    };	
	gcode->stream->printf("Total Calibration Successful\n");
	return true;
}

//Used to test the convergence of just the radius position adjustment
bool ZProbe::fix_delta_tower_radius(Gcode *gcode,int blame_tower)
{
	  if(verbose) gcode->stream->printf("Tower Radius Adjustment Testing\n");
      float target=0.03F;
	 	
	  BaseSolution::arm_options_t options;
      if(gcode->has_letter('I')) target= gcode->get_value('I'); //override default target
      if(gcode->has_letter('J')) this->probe_radius = gcode->get_value('J'); //override default probe radius
	  if(gcode->has_letter('A')) blame_tower=1;
	  if(gcode->has_letter('B')) blame_tower=2;
	  if(gcode->has_letter('C')) blame_tower=3;

      //get probe points
      float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y; 
      std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);
	  int dx, dy, dz,dax,day,daz;
	    home();
    // find bed, then move to a point 5mm above it
		int s;
		if(!run_probe(s, true)) return false;
		float bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed
		if(verbose) gcode->stream->printf("Bed ht is %f mm\n", bedht);

		home();
		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true);
        if(!probe_delta_tower(dx, t1x, t1y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TR:  A Z:%1.5f Steps:%d\n",  dx / Z_STEPS_PER_MM, dx);
        //if(!probe_delta_tower(daz,t6x,t6y)) return false;
        //if(verbose || verbose1) gcode->stream->printf("TR: -G Z:%1.5f Steps:%d\n", daz / Z_STEPS_PER_MM, daz);
        if(!probe_delta_tower(dy, t2x, t2y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TR:  B Z:%1.5f Steps:%d\n", dy / Z_STEPS_PER_MM, dy);
        //if(!probe_delta_tower(dax,t4x,t4y)) return false;
        //if(verbose || verbose1) gcode->stream->printf("TR: -A Z:%1.5f Steps:%d\n", dax / Z_STEPS_PER_MM, dax);
        if(!probe_delta_tower(dz, t3x, t3y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TR:  G Z:%1.5f Steps:%d\n", dz / Z_STEPS_PER_MM, dz);
        //if(!probe_delta_tower(day,t5x,t5y)) return false;
        //if(verbose || verbose1) gcode->stream->printf("TR: -B Z:%1.5f Steps:%d\n", day / Z_STEPS_PER_MM, day);
		
	  
	    //Begin tower radius adjustment
		//The tower radius option is only an adjustment factor
	    //Based on assumption that the direction of change in anti-tower distance is 
	    //inverse to the direction in change in the tower's radius adjustment
	    //ie increase a tower's radius decreases the anti-tower height.
	    int anti_tower,tower;
		float tower_radius,tower_radius_initial,tower_z;
	    float adjustment;//,step,previous_adjustment;
	    bool radius_done=false;
	    THEKERNEL->robot->arm_solution->get_optional(options);
		    //Set initial adjustment amount
	    //.5 if the anti-tower is 3x the average of the other anti_towers.
	    //otherwise it is a -.5*/
	    tower_radius=0;
		tower_radius_initial=tower_radius;
		adjustment=0;
		//Store current radius values in case of run-away condition
	    switch(blame_tower){
	    	case 1 : adjustment = (dax < dx) ? .5 : -.5;
	    		 tower_radius_initial= options['A'];
	    		 break;
	    	case 2 : adjustment = (day < dy) ? .5 : -.5;
	    		 tower_radius_initial= options['B'];
	    		 break;
	    	case 3 : adjustment = (daz < dy) ? .5 : -.5;
	    		 tower_radius_initial= options['C'];
	    };		
			float diff=999;
			float prev_diff;
	    do{
	    	//update the Delta Tower Radius 
	    	//Probe anti-tower positions
	    	switch(blame_tower) {
	    		case 1 : 
					options['A']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					tower_radius = options['A'];	    			 
					gcode->stream->printf("Alpha Radius Offset adjusted to %1.5f by %1.5f\n",tower_radius,adjustment);					
					calibrate_delta_endstops(gcode,false);
					home();
					//calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
					if(!probe_delta_tower(tower,t1x,t1y)) return false;
					gcode->stream->printf(" A Tower :%1.5f Steps:%d\n",tower / Z_STEPS_PER_MM, tower);
					if(!probe_delta_tower(anti_tower,t4x,t4y)) return false;
					gcode->stream->printf("-A Tower position:		 %1.5f Steps:%d\n",anti_tower / Z_STEPS_PER_MM, anti_tower);
					break;
	    		case 2 : 
					options['B']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					tower_radius = options['B'];
					gcode->stream->printf("Beta Radius Offset adjusted to %1.5f by %1.5f\n",tower_radius,adjustment);					
					calibrate_delta_endstops(gcode,false);
					home();					
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed					
					if(!probe_delta_tower(tower,t2x,t2y)) return false;
					gcode->stream->printf(" B Tower :%1.5f Steps:%d\n",tower / Z_STEPS_PER_MM, tower);
					if(!probe_delta_tower(anti_tower,t5x,t5y)) return false;
					gcode->stream->printf("-B Tower position:		 %1.5f Steps:%d\n",anti_tower / Z_STEPS_PER_MM, anti_tower);
					break;
	    		case 3 :
					options['C']+=adjustment;									
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					tower_radius = options['C'];
					gcode->stream->printf("Gamma Radius Offset adjusted to %1.5f by %1.5f\n",tower_radius,adjustment);
					calibrate_delta_endstops(gcode,false);
					home();
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
					if(!probe_delta_tower(tower,t3x,t3y)) return false;
					gcode->stream->printf(" G Tower :%1.5f Steps:%d\n",tower / Z_STEPS_PER_MM, tower);
					if(!probe_delta_tower(anti_tower,t6x,t6y)) return false;
					gcode->stream->printf("-G Tower position:		 %1.5f Steps:%d\n",anti_tower / Z_STEPS_PER_MM, anti_tower);
					break;
	    	};
	    	//Average the unblamed anti-tower positions
			tower_z=tower/Z_STEPS_PER_MM;
			prev_diff = diff;
			diff = ((anti_tower/Z_STEPS_PER_MM) - tower_z);			
			gcode->stream->printf("Off by %1.5f. Previously off by %1.5f \n",diff,prev_diff);
			/*if(abs(diff) > abs(prev_diff) ){
				gcode->stream->printf("Odd...Things got worse, it was previously off by %1.5f\n",prev_diff);
			}else{
			gcode->stream->printf("Look at that, things are getting better. It was previously off by %1.5f\n",prev_diff);
			};*/
	    	//Overshoot detection
	    	//half adjustment amount and reverse direction of change
	    	if((anti_tower/ Z_STEPS_PER_MM < tower_z) && (adjustment < 0)){
				gcode->stream->printf("Overshoot Detected\n");
				gcode->stream->printf("A-Tower(%1.5f)is less than Sides average %1.5f. Adjustment factor @ %1.5f\n",anti_tower / Z_STEPS_PER_MM, tower_z,adjustment);
				adjustment = -adjustment/4;
				gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
			};
			if((anti_tower/ Z_STEPS_PER_MM > tower_z) && (adjustment > 0)){
				gcode->stream->printf("Overshoot Detected\n");
				gcode->stream->printf("A-Tower(%1.5f)is greater than Sides average(%1.5f). Adjustment factor @ %1.5f\n",anti_tower / Z_STEPS_PER_MM, tower_z,adjustment);
				adjustment = -adjustment/4;
				gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
			};
			
			//set next adjustment
			//adjustment = (anti_tower < anti_t_average) ? adjustment : -adjustment;
	    	
			//Finished if within target
	    	if( ((anti_tower/ Z_STEPS_PER_MM) > (tower_z-target)) && ((anti_tower/ Z_STEPS_PER_MM) < (tower_z+ target)))
	    	{	
				adjustment = 0;	
				gcode->stream->printf("Radius Adjustment is satisfactory @ %f\n",tower_radius);
	    		radius_done=true;
	    	}
	    	if(abs(tower_radius_initial - tower_radius) > 10) 
	    	{gcode->stream->printf("Tower radius change exceeded limit\n");
	    	return false;
	    	};    	
	    } while(!radius_done);
		THEKERNEL->robot->arm_solution->get_optional(options);
		gcode->stream->printf("A%1.5f \t B%1.5f \t C%1.5f \t X%1.5f \t Y%1.5f \t Z%1.5f \t R%1.5f \t L%1.5f \n", options['A'],options['B'],options['C'],options['D'],options['E'],options['F'],options['R'],options['L']);
		home();
    	if(!run_probe(s, true)) return false;
		bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed
		gcode->stream->printf(" C \t 0 \t 0 \t %1.5f \n", bedht);
		home();
		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true);
		int dc;
		if(!probe_delta_tower(dc, 0, 0)) return false;
		gcode->stream->printf("CT\t %1.5f \n", dc / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dx, t1x, t1y)) return false;
		gcode->stream->printf(" A\t %1.5f \t %1.5f \t %1.5f\n",t1x,t1y,dx / Z_STEPS_PER_MM);
		if(!probe_delta_tower(daz,t6x,t6y)) return false;
		gcode->stream->printf("-G\t %1.5f \t %1.5f \t %1.5f\n",t6x,t6y, daz / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dy, t2x, t2y)) return false;
		gcode->stream->printf(" B\t %1.5f \t %1.5f \t %1.5f\n",t2x,t2y, dy / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dax,t4x,t4y)) return false;
		gcode->stream->printf("-A\t %1.5f \t %1.5f \t %1.5f\n",t4x,t4y, dax / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dz, t3x, t3y)) return false;
		gcode->stream->printf(" G\t %1.5f \t %1.5f \t %1.5f\n",t3x,t3y, dz / Z_STEPS_PER_MM);
		if(!probe_delta_tower(day,t5x,t5y)) return false;
		gcode->stream->printf("-B\t %1.5f \t %1.5f \t %1.5f\n",t5x,t5y, day / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dx, t1x/2, t1y/2)) return false;
		gcode->stream->printf(" A/2\t %1.5f \t %1.5f \t %1.5f\n",t1x/2,t1y/2,  dx / Z_STEPS_PER_MM);
		if(!probe_delta_tower(daz,t6x/2,t6y/2)) return false;
		gcode->stream->printf("-G/2\t %1.5f \t %1.5f \t %1.5f\n",t6x/2,t6y/2, daz / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dy, t2x/2, t2y/2)) return false;
		gcode->stream->printf(" B/2\t %1.5f \t %1.5f \t %1.5f\n",t2x/2,t2y/2, dy / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dax,t4x/2,t4y/2)) return false;
		gcode->stream->printf("-A/2\t %1.5f \t %1.5f \t %1.5f\n",t4x/2,t4y/2, dax / Z_STEPS_PER_MM);
		if(!probe_delta_tower(dz, t3x/2, t3y/2)) return false;
		gcode->stream->printf(" G/2\t %1.5f \t %1.5f \t %1.5f\n",t3x/2,t3y/2, dz / Z_STEPS_PER_MM);
		if(!probe_delta_tower(day,t5x/2,t5y/2)) return false;
		gcode->stream->printf("-B/2\t %1.5f \t %1.5f \t %1.5f\n",t5x/2,t5y/2, day / Z_STEPS_PER_MM);
        if(!probe_delta_tower(dx, t1x/2, t1y/2)) return false;
		return true;
};

//Used to test the convergence of just the angular position adjustment
bool ZProbe::fix_delta_tower_position(Gcode *gcode, int blame_tower)
{
	  if(verbose) gcode->stream->printf("Tower Delta Adjustment Testing\n");
      float target=0.03F;
	  //int blame_tower=	0;
	  float bedht;
	  int dx, dy, dz,dax,day,daz;	  
	  BaseSolution::arm_options_t options;
      if(gcode->has_letter('I')) target= gcode->get_value('I'); //override default target
      if(gcode->has_letter('J')) this->probe_radius = gcode->get_value('J'); //override default probe radius
	  if(gcode->has_letter('X')) blame_tower=1;
	  if(gcode->has_letter('Y')) blame_tower=2;
	  if(gcode->has_letter('Z')) blame_tower=3;

      //get probe points
      float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y; 
      std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);
	  
	    //Begin tower radius adjustment
		//The tower radius option is only an adjustment factor
	    //Based on assumption that the direction of change in anti-tower distance is 
	    //inverse to the direction in change in the tower's radius adjustment
	    //ie increase a tower's radius decreases the anti-tower height.
	    int anti_tower_left,anti_tower_right;
	    float adjustment,step,previous_adjustment;
	    
	    THEKERNEL->robot->arm_solution->get_optional(options);
	    //int dx, dy, dz,dax,day,daz,s;
		int s;
		float diff=999;
		float prev_diff = 0;
	//Begin Tower Angular Adjustment		
		if(verbose) gcode->stream->printf("Starting Angular Adjustment test\n");
	    adjustment = 0;
		//step is the amount the tower's angular coordinate will be changed
		 step=.5;
		//angular position adjustment loop
		do{
			//find start position
			/*home();
			// find bed, then move to a point 5mm above it    
			if(!run_probe(s, true)) return false;    
			bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
			//gcode->stream->printf("Bed ht is %f mm\n", bedht);    
      		home();
			coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
			*/
			//probe anti-tower positions next to blamed tower
	    	switch(blame_tower) {
	    		case 1 :
					options['X']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					gcode->stream->printf("Alpha Tower's angle adjusted to %1.5f by %1.5f\n",options['X'],adjustment);				
					home();
					//calibrate_delta_endstops(gcode,false);
					//calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed						
					if(!probe_delta_tower(anti_tower_right,t6x,t6y)) return false;
					gcode->stream->printf("Right Midpoint:\t %1.5f \t Steps:%d\n", anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
	    			if(!probe_delta_tower(anti_tower_left ,t5x,t5y)) return false;
					gcode->stream->printf("Left Midpoint:\t %1.5f \t Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
						
					break;
	    		case 2 :
					options['Y']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options); 
					gcode->stream->printf("Beta Tower's angle adjusted to %1.5f by %1.5f\n",options['Y'],adjustment);
					home();
					//calibrate_delta_endstops(gcode,false);
					//calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed						
					if(!probe_delta_tower(anti_tower_right,t4x,t4y)) return false;
					gcode->stream->printf("Right Midpoint:\t %1.5f \t Steps:%d\n", anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
	    			if(!probe_delta_tower(anti_tower_left ,t6x,t6y)) return false;
					gcode->stream->printf("Left Midpoint:\t %1.5f \t Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
					
					break;
	    		case 3 :
					options['Z']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					gcode->stream->printf("Gamma Tower's angle adjusted to %1.5f by %1.5f\n",options['Z'],adjustment);
					home();//home to reset arm solution
					//calibrate_delta_endstops(gcode,false);
					//calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed						
					if(!probe_delta_tower(anti_tower_right,t5x,t5y)) return false;
					gcode->stream->printf("Right Midpoint:\t %1.5f \t Steps:%d\n", anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
					if(!probe_delta_tower(anti_tower_left ,t4x,t4y)) return false;
					gcode->stream->printf("Left Midpoint:\t %1.5f \t Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
					
					break;
	    	};//end of switch statement
			
	    	//Save previous adjustment and reset adjustment
	    	previous_adjustment = adjustment;
	    	adjustment = 0;
			prev_diff = diff;
			diff = anti_tower_left - anti_tower_right;	
			diff = diff/Z_STEPS_PER_MM;
			gcode->stream->printf("Difference:%1.5f \n",diff);			
			if(abs(diff) > abs(prev_diff) ){
				gcode->stream->printf("Odd...Things got worse, it was previously off by %1.5f\n",prev_diff);
			}else{
			gcode->stream->printf("Look at that, things are getting better!\n");
			};
	    	//set adjustment amounts
			if((anti_tower_left/ Z_STEPS_PER_MM + target) > (anti_tower_right/ Z_STEPS_PER_MM)) adjustment=step;
	    	if((anti_tower_left/ Z_STEPS_PER_MM - target) < anti_tower_right/ Z_STEPS_PER_MM) adjustment=-step;
			
			//Check completion criteria
			if( abs(diff) <= target) {
				adjustment=0;
				gcode->stream->printf("Angle Adjustment is satisfactory \n");
			};
	    	//detect and correct low overshoot
	    	if((adjustment > 0) && (previous_adjustment <0)){
				gcode->stream->printf("Overshoot Detected\n");
				gcode->stream->printf("Adjustment was %1.5f and now is %1.5f\n",previous_adjustment,adjustment);
				gcode->stream->printf("A-Tower Left(%1.5f)is greater than Anti_tower Right(%1.5f).Moving right (pos) @ adjustment factor(%1.5f)\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_right / Z_STEPS_PER_MM,adjustment);
				adjustment = adjustment/4;
				step = step /4;
				gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
	    	};
			//detect and correct high overshoot
			if((adjustment < 0) && (previous_adjustment >0)){
				gcode->stream->printf("Overshoot Detected\n");
				gcode->stream->printf("Adjustment was %1.5f and now is %1.5f\n",previous_adjustment,adjustment);
				gcode->stream->printf("A-Tower Left(%1.5f)is less than A-Tower Right(%1.5f). Moving left (neg) @ adjustment factor(%1.5f)\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_right / Z_STEPS_PER_MM,adjustment);
				adjustment = adjustment/4;
				step = step /4;
				gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
			};
	    } while(adjustment !=0);	    
    	THEKERNEL->robot->arm_solution->get_optional(options);
		if(verbose|| verbose1) gcode->stream->printf("A%1.5f \t B%1.5f \t G%1.5f \t R%1.5f \t L%1.5f \t X%1.5f \t Y%1.5f \t Z%1.5f \n", options['A'],options['B'],options['C'],options['R'],options['L'],options['X'],options['Y'],options['Z']);
		home();
    	if(!run_probe(s, true)) return false;
		bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed
		if(verbose|| verbose1) gcode->stream->printf("TD: C \t 0 \t 0 \t %f Steps:%d\n", bedht,s);
		home();
		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true);
		int dc;
		if(!probe_delta_tower(dc, 0, 0)) return false;
		if(verbose) gcode->stream->printf("TD: CT\t Z:%1.5f \t Steps:%d\n", dc / Z_STEPS_PER_MM, dc);
        if(!probe_delta_tower(dx, t1x, t1y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD:  A \t %1.5f %1.5f %1.5f Steps:%d\n",t1x,t1y,  dx / Z_STEPS_PER_MM, dx);
        if(!probe_delta_tower(daz,t6x,t6y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD: -G \t %1.5f %1.5f %1.5f Steps:%d\n",t6x,t6y, daz / Z_STEPS_PER_MM, daz);
        if(!probe_delta_tower(dy, t2x, t2y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD:  B \t %1.5f %1.5f %1.5f Steps:%d\n",t2x,t2y, dy / Z_STEPS_PER_MM, dy);
        if(!probe_delta_tower(dax,t4x,t4y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD: -A \t %1.5f %1.5f %1.5f Steps:%d\n",t4x,t4y, dax / Z_STEPS_PER_MM, dax);
        if(!probe_delta_tower(dz, t3x, t3y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD:  G \t %1.5f %1.5f %1.5f Steps:%d\n",t3x,t3y, dz / Z_STEPS_PER_MM, dz);
        if(!probe_delta_tower(day,t5x,t5y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD: -B \t %1.5f %1.5f %1.5f Steps:%d\n",t5x,t5y, day / Z_STEPS_PER_MM, day);
	
        if(!probe_delta_tower(dx, t1x, t1y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD:  A/2 \t %1.5f %1.5f %1.5f Steps:%d\n",t1x/2,t1y/2,  dx / Z_STEPS_PER_MM, dx);
        if(!probe_delta_tower(daz,t6x,t6y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD: -G/2 \t %1.5f %1.5f %1.5f Steps:%d\n",t6x/2,t6y/2, daz / Z_STEPS_PER_MM, daz);
        if(!probe_delta_tower(dy, t2x, t2y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD:  B/2 \t %1.5f %1.5f %1.5f Steps:%d\n",t2x/2,t2y/2, dy / Z_STEPS_PER_MM, dy);
        if(!probe_delta_tower(dax,t4x,t4y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD: -A/2 \t %1.5f %1.5f %1.5f Steps:%d\n",t4x/2,t4y/2, dax / Z_STEPS_PER_MM, dax);
        if(!probe_delta_tower(dz, t3x, t3y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD:  G/2 \t %1.5f %1.5f %1.5f Steps:%d\n",t3x/2,t3y/2, dz / Z_STEPS_PER_MM, dz);
        if(!probe_delta_tower(day,t5x,t5y)) return false;
        if(verbose || verbose1) gcode->stream->printf("TD: -B/2 \t %1.5f %1.5f %1.5f Steps:%d\n",t5x/2,t5y/2, day / Z_STEPS_PER_MM, day);		
return true;
};

//Marlin based implementation of Tower angle and radius correction
bool ZProbe::calibrate_delta_tower_position(Gcode *gcode,int suggested_tower)
{		
	  if(verbose) gcode->stream->printf("Calibrating Tower Radius and Angle\n");
	  //setup data
      float target=0.03F;
      if(gcode->has_letter('I')) target= gcode->get_value('I'); //override default target
      if(gcode->has_letter('J')) this->probe_radius = gcode->get_value('J'); //override default probe radius
      
	  int dx, dy, dz,dax,day,daz;
      //get probe points
      float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y; 
      std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);
      int s;
	  float bedht;
	  BaseSolution::arm_options_t options;
	  
      //options.clear();*/
      float alpha,beta,gamma;
      bool alpha_bad,beta_bad,gamma_bad;
      alpha_bad=true;
      beta_bad=true;
      gamma_bad=true;
      int blame_tower;
	  
	  //Stop correcting towers if it hasn't converged after 20 loops
      for (int i = 1; i <= 20; ++i) {
		home();
		// find bed, then move to a point 5mm above it    
		if(!run_probe(s, true)) return false;    
		bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
		gcode->stream->printf("Bed ht is %f mm\n", bedht);    
      	home();
		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
		
		//Determine tower to blame and focus the next corrections on
		//	-skip this probe sequence if passed a tower to blame already
		if(!suggested_tower==1 ||!suggested_tower==2 || !suggested_tower==3) //only skip if passed a legitimate tower selection
		{
		//gcode->stream->printf("Testing 6 Point Positions\n");
        // probe towers and anti-tower positions using coordinated moves

        if(!probe_delta_tower(dx, t1x, t1y)) return false;
        gcode->stream->printf("Pass-%d Alpha:%1.5f Steps:%d\n", i, dx / Z_STEPS_PER_MM, dx);
        if(!probe_delta_tower(daz,t6x,t6y)) return false;
        gcode->stream->printf("Pass-%d AntiGamma:%1.5f Steps:%d\n", i, daz / Z_STEPS_PER_MM, daz);
        if(!probe_delta_tower(dy, t2x, t2y)) return false;
        gcode->stream->printf("Pass-%d Beta:%1.5f Steps:%d\n", i, dy / Z_STEPS_PER_MM, dy);
        if(!probe_delta_tower(dax,t4x,t4y)) return false;
        gcode->stream->printf("Pass-%d AntiAlpha:%1.5f Steps:%d\n", i, dax / Z_STEPS_PER_MM, dax);
        if(!probe_delta_tower(dz, t3x, t3y)) return false;
        gcode->stream->printf("Pass-%d Gamma:%1.5f Steps:%d\n", i, dz / Z_STEPS_PER_MM, dz);
        if(!probe_delta_tower(day,t5x,t5y)) return false;
        gcode->stream->printf("Pass-%d AntiBeta:%1.5f Steps:%d\n", i, day / Z_STEPS_PER_MM, day);
            
        //get difference of tower and anti-tower positions
        alpha= (dx - dax);
		gcode->stream->printf("Alpha and -Alpha difference: %1.2f \n",alpha/ Z_STEPS_PER_MM);
        beta = (dy - day);
		gcode->stream->printf("Beta and -Beta difference:   %1.2f\n",beta/ Z_STEPS_PER_MM);
        gamma= (dz - daz);
		gcode->stream->printf("Gamma and -Gamma difference: %1.2f\n",gamma/ Z_STEPS_PER_MM);
        //reset tower flags
		alpha_bad=true;
		beta_bad=true;
		gamma_bad=true;
	
		 //decide which tower is worst
        auto mm1 = minmax({alpha,beta,gamma},abs_compare);
        if(abs(alpha/ Z_STEPS_PER_MM) <= target) alpha_bad = false;
        if(abs(beta / Z_STEPS_PER_MM) <= target) beta_bad  = false;
        if(abs(gamma/ Z_STEPS_PER_MM )<= target) gamma_bad = false;
        //decide which tower to blame
        blame_tower=-1;
        if(alpha==mm1.second) blame_tower=1;
		if(beta==mm1.second) blame_tower=2;
		if(gamma==mm1.second) blame_tower=3;
		//if(	 alpha_bad &&  beta_bad &&  gamma_bad) blame_tower = 0; //Possible they are all off, already picked worst tower
        if( !alpha_bad &&  beta_bad &&  gamma_bad) blame_tower = 1;
        if(  alpha_bad && !beta_bad &&  gamma_bad) blame_tower = 2;
        if(  alpha_bad &&  beta_bad && !gamma_bad) blame_tower = 3;
        if(  alpha_bad && !beta_bad && !gamma_bad) blame_tower = 1;
        if(  alpha_bad &&  beta_bad && !gamma_bad) blame_tower = 2;
        if(  alpha_bad && !beta_bad &&  gamma_bad) blame_tower = 3;
		gcode->stream->printf("Blaming tower: %i\n",blame_tower);
        if(alpha_bad){ gcode->stream->printf("Alpha radius off ");}
	    	else gcode->stream->printf("Alpha radius Good ");	
	    if(beta_bad){ gcode->stream->printf("Beta radius off ");}
	    	else gcode->stream->printf("Beta radius Good ");	
	    if(gamma_bad){ gcode->stream->printf("Gamma radius off\n");}
		else	gcode->stream->printf("Gamma radius Good\n");	
	    gcode->stream->printf("Blaming tower:%d\n",blame_tower);
	    }
		else	//don't skip after the first time
		{	
			blame_tower = suggested_tower;
			suggested_tower =0;
		};		
	
	
//Begin tower radius adjustment
	 if(verbose) gcode->stream->printf("Adjusting Tower Radius\n");
/*		//The tower radius option is only an adjustment factor
	    //Based on assumption that the direction of change in anti-tower distance is 
	    //inverse to the direction in change in the tower's radius adjustment
	    //ie increase a tower's radius decreases the anti-tower height.*/
	    int anti_tower,anti_tower_left,anti_tower_right;
		float tower_radius,tower_radius_initial,anti_t_average;
	    float adjustment,step,previous_adjustment;
	    bool radius_done=false;
	    THEKERNEL->robot->arm_solution->get_optional(options);
		float diff=999;
		float prev_diff;
	    
/*	    //Set initial adjustment amount
	    //.5 if the anti-tower is 3x the average of the other anti_towers.
	    //otherwise it is a -.5*/
	    tower_radius=0;
		tower_radius_initial=tower_radius;
		adjustment=0;
		//Store current radius values in case of run-away condition
	    switch(blame_tower){
	    	case 1 : adjustment = (3*dax < (day+daz)/2) ? .5 : -.5;
	    		 tower_radius_initial= options['A'];
	    		 break;
	    	case 2 : adjustment = (3*day < (dax+daz)/2) ? .5 : -.5;
	    		 tower_radius_initial= options['B'];
	    		 break;
	    	case 3 : adjustment = (3*daz < (dax+day)/2) ? .5 : -.5;
	    		 tower_radius_initial= options['C'];
	    };
	    //radius adjustment loop
		do{
	    	//update the Delta Tower Radius 
	    	//Probe anti-tower positions
	    	switch(blame_tower) {
	    		case 1 : 
					options['A']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					tower_radius = options['A'];	    			 
					gcode->stream->printf("Alpha Radius Offset adjusted to %1.5f by %1.5f\n",tower_radius,adjustment);					
					home();
					calibrate_delta_endstops(gcode,false);
					calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
					if(!probe_delta_tower(anti_tower,t4x,t4y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower position:		 %1.5f Steps:%d\n",anti_tower / Z_STEPS_PER_MM, anti_tower);
					if(!probe_delta_tower(anti_tower_right,t5x,t5y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower Right midpoint:%1.5f Steps:%d\n",anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
					if(!probe_delta_tower(anti_tower_left,t6x,t6y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower Left midpoint:%1.5f Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
					break;
	    		case 2 : 
					options['B']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					tower_radius = options['B'];
					gcode->stream->printf("Beta Radius Offset adjusted to %1.5f by %1.5f\n",tower_radius,adjustment);					
					home();
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed					
					if(!probe_delta_tower(anti_tower,t5x,t5y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower position:	 	 %1.5f Steps:%d\n",anti_tower / Z_STEPS_PER_MM, anti_tower);
					if(!probe_delta_tower(anti_tower_right,t6x,t6y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower Right midpoint:%1.5f Steps:%d\n",anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
					if(!probe_delta_tower(anti_tower_left,t4x,t4y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower Left midpoint:%1.5f Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
					break;
	    		case 3 :
					options['C']+=adjustment;									
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					tower_radius = options['C'];
					gcode->stream->printf("Gamma Radius Offset adjusted to %1.5f by %1.5f\n",tower_radius,adjustment);
					home();
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
					if(!probe_delta_tower(anti_tower,t6x,t6y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower position:		 %1.5f Steps:%d\n",anti_tower / Z_STEPS_PER_MM, anti_tower);
					if(!probe_delta_tower(anti_tower_right,t4x,t4y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower Right midpoint:%1.5f Steps:%d\n",anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
					if(!probe_delta_tower(anti_tower_left,t5x,t5y)) return false;
						 if(verbose) gcode->stream->printf("A-Tower Left midpoint:%1.5f Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
					break;
	    	};
	    	//Average the unblamed anti-tower positions
	    	anti_t_average= (anti_tower_left + anti_tower_right);
			anti_t_average= anti_t_average/2;
			anti_t_average= anti_t_average/Z_STEPS_PER_MM;
			prev_diff = diff;
			diff = ((anti_tower/Z_STEPS_PER_MM) - anti_t_average);			
				 if(verbose) gcode->stream->printf("Off by %1.5f from Left & Right Avg (%1.5f)\n",diff,anti_t_average);
			if(verbose){if(abs(diff) > abs(prev_diff) ){
				gcode->stream->printf("Odd...Things got worse, it was previously off by %1.5f\n",prev_diff);
			}else{
			gcode->stream->printf("Look at that, things are getting better. It was previously off by %1.5f\n",prev_diff);
			};};
	    	//Overshoot detection
	    	//half adjustment amount and reverse direction of change
	    	if((anti_tower/ Z_STEPS_PER_MM < anti_t_average) && (adjustment < 0)){
					 if(verbose) gcode->stream->printf("Overshoot Detected\n");
					 if(verbose) gcode->stream->printf("A-Tower(%1.5f)is less than Sides average %1.5f. Adjustment factor @ %1.5f\n",anti_tower / Z_STEPS_PER_MM, anti_t_average,adjustment);
				adjustment = -adjustment/2;
					 if(verbose) gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
			};
			if((anti_tower/ Z_STEPS_PER_MM > anti_t_average) && (adjustment > 0)){
					 if(verbose) gcode->stream->printf("Overshoot Detected\n");
					 if(verbose) gcode->stream->printf("A-Tower(%1.5f)is greater than Sides average(%1.5f). Adjustment factor @ %1.5f\n",anti_tower / Z_STEPS_PER_MM, anti_t_average,adjustment);
				adjustment = -adjustment/2;
					 if(verbose) gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
			};
			
			//set next adjustment
			//adjustment = (anti_tower < anti_t_average) ? adjustment : -adjustment;
	    	
			//Finished if within target
	    	if( ((anti_tower/ Z_STEPS_PER_MM) > (anti_t_average-target)) && ((anti_tower/ Z_STEPS_PER_MM) < (anti_t_average+ target)))
	    	{	
				adjustment = 0;	
				gcode->stream->printf("Radius Adjustment is satisfactory\n");
	    		radius_done=true;
	    	}
	    	if(abs(tower_radius_initial - tower_radius) > 10) 
	    	{gcode->stream->printf("Tower radius change exceeded limit\n");
	    	return false;
	    	};
			
	    
	    	
	    } while(!radius_done);
		

//Begin Tower Angular Adjustment		
		//reset values
	    adjustment = 0;
		diff=999;
		prev_diff = 0;
		//step is the amount the tower's angular coordinate will be changed
		step=.5;
		//angular position adjustment loop
		do{
			//probe anti-tower positions next to blamed tower
	    	switch(blame_tower) {
	    		case 1 :
					options['X']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					gcode->stream->printf("Alpha Tower's angle adjusted to %1.5f by %1.5f\n",options['X'],adjustment);				
					home();
					calibrate_delta_endstops(gcode,false);
					calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed						
					if(!probe_delta_tower(anti_tower_right,t6x,t6y)) return false;
					if(verbose) gcode->stream->printf("Right Midpoint:%1.5f Steps:%d\n", anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
	    			if(!probe_delta_tower(anti_tower_left ,t5x,t5y)) return false;
					if(verbose) gcode->stream->printf("Left Midpoint:%1.5f Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
						
					break;
	    		case 2 :
					options['Y']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options); 
					gcode->stream->printf("Beta Tower's angle adjusted to %1.5f by %1.5f\n",options['Y'],adjustment);
					home();
					calibrate_delta_endstops(gcode,false);
					calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed						
					if(!probe_delta_tower(anti_tower_right,t4x,t4y)) return false;
					if(verbose) gcode->stream->printf("Right Midpoint:%1.5f Steps:%d\n", anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
	    			if(!probe_delta_tower(anti_tower_left ,t6x,t6y)) return false;
					if(verbose) gcode->stream->printf("Left Midpoint:%1.5f Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
					
					break;
	    		case 3 :
					options['Z']+=adjustment;
					THEKERNEL->robot->arm_solution->set_optional(options);
					THEKERNEL->robot->arm_solution->get_optional(options);
					gcode->stream->printf("Gamma Tower's angle adjusted to %1.5f by %1.5f\n",options['Z'],adjustment);
					home();//home to reset arm solution
					calibrate_delta_endstops(gcode,false);
					calibrate_delta_radius(gcode);
					// find bed, then move to a point 5mm above it    
					if(!run_probe(s, true)) return false;    
					bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
					home();
					coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed						
					if(!probe_delta_tower(anti_tower_right,t5x,t5y)) return false;
					if(verbose) gcode->stream->printf("Right Midpoint:%1.5f Steps:%d\n", anti_tower_right / Z_STEPS_PER_MM, anti_tower_right);
					if(!probe_delta_tower(anti_tower_left ,t4x,t4y)) return false;
					if(verbose) gcode->stream->printf("Left Midpoint:%1.5f Steps:%d\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_left);
					
					break;
	    	};//end of switch statement
			
	    	//Save previous adjustment and reset adjustment
	    	previous_adjustment = adjustment;
	    	adjustment = 0;
			prev_diff = diff;
			diff = anti_tower_left - anti_tower_right;	
			diff = diff/Z_STEPS_PER_MM;
			gcode->stream->printf("Difference:%1.5f \n",diff);			
			if(verbose){if(abs(diff) > abs(prev_diff) ){
				gcode->stream->printf("Odd...Things got worse, it was previously off by %1.5f\n",prev_diff);
			}else{
			gcode->stream->printf("Look at that, things are getting better!\n");
			};};
	    	//set adjustment amounts
			if((anti_tower_left/ Z_STEPS_PER_MM + target) > (anti_tower_right/ Z_STEPS_PER_MM)) adjustment=step;
	    	if((anti_tower_left/ Z_STEPS_PER_MM - target) < anti_tower_right/ Z_STEPS_PER_MM) adjustment=-step;
			
			//Check completion criteria
			if( abs(diff) <= target) {
				adjustment=0;
				if(verbose) gcode->stream->printf("Angle Adjustment is satisfactory \n");
			};
	    	//detect and correct low overshoot
	    	if((adjustment > 0) && (previous_adjustment <0)){
				if(verbose) gcode->stream->printf("Overshoot Detected\n");
				if(verbose) gcode->stream->printf("Adjustment was %1.5f and now is %1.5f\n",previous_adjustment,adjustment);
				if(verbose) gcode->stream->printf("A-Tower Left(%1.5f)is greater than Anti_tower Right(%1.5f).Moving right (pos) @ adjustment factor(%1.5f)\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_right / Z_STEPS_PER_MM,adjustment);
				adjustment = adjustment/2;
				step = step /2;
					 if(verbose) gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
	    	};
			//detect and correct high overshoot
			if((adjustment < 0) && (previous_adjustment >0)){
				if(verbose) gcode->stream->printf("Overshoot Detected\n");
				if(verbose) gcode->stream->printf("Adjustment was %1.5f and now is %1.5f\n",previous_adjustment,adjustment);
				if(verbose) gcode->stream->printf("A-Tower Left(%1.5f)is less than A-Tower Right(%1.5f). Moving left (neg) @ adjustment factor(%1.5f)\n",anti_tower_left / Z_STEPS_PER_MM, anti_tower_right / Z_STEPS_PER_MM,adjustment);
				adjustment = adjustment/2;
				step = step /2;
					 if(verbose) gcode->stream->printf("changing adjustment factor to %1.5f\n",adjustment);
			};
	    } while(adjustment !=0);	    
    } return true;//end of calibration loop              	
}

//Routine that gathers lots of points to assess movement consistency 
bool ZProbe::assess_consistancy(Gcode *gcode)
{	
	gcode->stream->printf("Starting Consistency assessment\n");
	//get probe points
    float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y; 
    std::tie(t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y, t5x, t5y, t6x, t6y) = getCoordinates(this->probe_radius);
	BaseSolution::arm_options_t options;
	THEKERNEL->robot->arm_solution->get_optional(options);
	
	int s,sample_size;
	sample_size=20;
	int temp=0;
	
	temp=gcode->get_value('P');
	if( temp<100 && temp>0) sample_size=temp;
	temp=0;
	float bedht; 
	
	vector<int> test_point;
		if(!run_probe(s, true)) return false;
		bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
		gcode->stream->printf("Bed ht is %f mm\n", bedht);
		home();
		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true);    	
/*	for (int i = 0; i < 20; ++i){ 
	home();
		// find bed, then move to a point 5mm above it    
		if(!run_probe(s, true)) return false;
		bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
		gcode->stream->printf("Bed ht is %f mm\n", bedht);    
      	home();
		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
		//gcode->stream->printf("Testing 6 Point Positions\n");
        // probe towers and anti-tower positions using coordinated moves
        if(!probe_delta_tower(temp, t1x, t1y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d Alpha:%1.5f Steps:%d\n", i, temp / Z_STEPS_PER_MM, dx);
        if(!probe_delta_tower(temp,t6x,t6y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d AntiGamma:%1.5f Steps:%d\n", i, daz / Z_STEPS_PER_MM, daz);
        if(!probe_delta_tower(temp, t2x, t2y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d Beta:%1.5f Steps:%d\n", i, dy / Z_STEPS_PER_MM, dy);
        if(!probe_delta_tower(temp,t4x,t4y)) return false;
		test_point.push_back(temp);
		//gcode->stream->printf("Sample-%d AntiAlpha:%1.5f Steps:%d\n", i, dax / Z_STEPS_PER_MM, dax);
        if(!probe_delta_tower(temp, t3x, t3y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d Gamma:%1.5f Steps:%d\n", i, dz / Z_STEPS_PER_MM, dz);
        if(!probe_delta_tower(temp,t5x,t5y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d AntiBeta:%1.5f Steps:%d\n", i, day / Z_STEPS_PER_MM, day);
		};
		
		//get the mean
		int sum=0;
		
		int n = test_point.size();
	
		for ( int i =0; i < n; i++)
		{
			sum += test_point[i];
		};
		float mean = sum / n;
		//simple variance
	//get the variance
		sum=0;
		float temp2=0.0;
		float variance=0.0;
		float size = test_point.size();
		for ( int i = 0; i< size; i++)
		{	
			temp2 = test_point[i] - mean;
			temp2 = temp2 * temp2;
			sum += temp2;
		};
		variance = sum /(test_point.size()-2);
*/
/*
		// compensated variance
		float sum2=0;
		float sum3=0;
		for( int i=0; i < n; i++)
		{
			sum2 = sum2 + pow((test_point[i] - mean),2);
			sum3 = sum3 + (test_point[i] - mean);
		};
		float variance = (sum2 - (pow(sum3,2)/n))/(n-1);
		float deviation = sqrt(variance);
		gcode->stream->printf("Bed-height:%1.5f Mean: %1.5f Variance: %1.5f STD Deviation:%1.5f Sample Size:%d",bedht,mean/Z_STEPS_PER_MM ,variance/Z_STEPS_PER_MM ,deviation/Z_STEPS_PER_MM ,n);		
			
*/	//alternate version
	for (int i = 0; i < sample_size; ++i){ 
//		home();
		// find bed, then move to a point 5mm above it    
//		if(!run_probe(s, true)) return false;
//		bedht= s/Z_STEPS_PER_MM - this->probe_height; // distance to move from home to 5mm above bed    
//		gcode->stream->printf("Bed ht is %f mm\n", bedht);    
//      home();
//		coordinated_move(NAN, NAN, -bedht, this->fast_feedrate, true); // do a relative move from home to the point above the bed
		//gcode->stream->printf("Testing 6 Point Positions\n");
        // probe towers and anti-tower positions using coordinated moves
        if(!probe_delta_tower(temp, t1x, t1y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d Alpha:%1.5f Steps:%d\n", i, temp / Z_STEPS_PER_MM, temp);
        if(!probe_delta_tower(temp,t6x,t6y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d AntiGamma:%1.5f Steps:%d\n", i, temp / Z_STEPS_PER_MM, temp);
        if(!probe_delta_tower(temp, t2x, t2y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d Beta:%1.5f Steps:%d\n", i, temp / Z_STEPS_PER_MM, temp);
        if(!probe_delta_tower(temp,t4x,t4y)) return false;
		test_point.push_back(temp);
		//gcode->stream->printf("Sample-%d AntiAlpha:%1.5f Steps:%d\n", i, temp / Z_STEPS_PER_MM, temp);
        if(!probe_delta_tower(temp, t3x, t3y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d Gamma:%1.5f Steps:%d\n", i, temp / Z_STEPS_PER_MM, temp);
        if(!probe_delta_tower(temp,t5x,t5y)) return false;
		test_point.push_back(temp);
        //gcode->stream->printf("Sample-%d AntiBeta:%1.5f Steps:%d\n", i, temp / Z_STEPS_PER_MM, temp);
		};
		
		//get the sum
		vector<int> sum (6,0);
		gcode->stream->printf(" %d Sum vector [0]=%d",int(sum.size()),sum[5]);
		int n = test_point.size();
	
		for ( int i =0; i < n; i=i+6)
		{
			//gcode->stream->printf("Sum[0]=%d",sum[0]);
			sum[0] += test_point[i];
			//gcode->stream->printf("+%d = %d",test_point[i],sum[0]);
			//gcode->stream->printf("Sum[0]=%d",sum[1]);
			sum[1] += test_point[i+1];
			//gcode->stream->printf("+%d = %d",test_point[i],sum[1]);
			//gcode->stream->printf("Sum[0]=%d",sum[2]);
			sum[2] += test_point[i+2];
			//gcode->stream->printf("+%d = %d",test_point[i],sum[2]);
			//gcode->stream->printf("Sum[0]=%d",sum[3]);
			sum[3] += test_point[i+3];
			//gcode->stream->printf("+%d = %d",test_point[i],sum[3]);
			//gcode->stream->printf("Sum[0]=%d",sum[4]);
			sum[4] += test_point[i+4];
			//gcode->stream->printf("+%d = %d",test_point[i],sum[4]);
			//gcode->stream->printf("Sum[0]=%d",sum[5]);
			sum[5] += test_point[i+5];
			//gcode->stream->printf("+%d = %d",test_point[i],sum[5]);
		};
		//compute the mean
		vector<float> mean (6,0.0) ;
		for (int i =0; i < 6; i++)
		{
			mean[i]= (sum[i]/(n/6));
			//gcode->stream->printf("Mean[%d]=%1.5f\n",i,mean[i]);
		};
				
		for( int offset =0; offset < 6; offset++){
		// compensated variance
		float sum2=0;
		float sum3=0;
		
		for( int i=offset; i < n; i=i+6)
		{
			float temp = float(test_point[i]);
			sum2 = sum2 + pow((temp - mean[offset]),2);
			sum3 = sum3 + (temp - mean[offset]);
		};
		float variance = (sum2 - (pow(sum3,2)/n))/(n-1);
		float deviation = sqrt(variance);
		gcode->stream->printf("Test Point %d: Mean: %1.5f Variance: %1.5f STD Deviation:%1.5f\n",offset,mean[offset]/Z_STEPS_PER_MM ,variance/Z_STEPS_PER_MM ,deviation/Z_STEPS_PER_MM);
		};
		
						
		return true;
	
};

void ZProbe::on_gcode_received(void *argument)
{
    Gcode *gcode = static_cast<Gcode *>(argument);

    if( gcode->has_g) {
        // G code processing
        if( gcode->g == 30 ) { // simple Z probe
            gcode->mark_as_taken();
            // first wait for an empty queue i.e. no moves left
            THEKERNEL->conveyor->wait_for_empty_queue();

            // make sure the probe is not already triggered before moving motors
            if(this->pin.get()) {
                gcode->stream->printf("ZProbe triggered before move, aborting command.\n");
                return;
            }

            int steps;
            if(run_probe(steps)) {
                gcode->stream->printf("Z:%1.4f C:%d\n", steps / Z_STEPS_PER_MM, steps);
                // move back to where it started, unless a Z is specified
                if(gcode->has_letter('Z')) {
                    // set Z to the specified value, and leave probe where it is
                    THEKERNEL->robot->reset_axis_position(gcode->get_value('Z'), Z_AXIS);
                } else {
                    return_probe(steps);
                }
            } else {
                gcode->stream->printf("ZProbe not triggered\n");
            }
		} else if( gcode->g == 32 ) { // auto calibration for delta, Z bed mapping for cartesian
            // first wait for an empty queue i.e. no moves left
            THEKERNEL->conveyor->wait_for_empty_queue();
            gcode->mark_as_taken();

            // make sure the probe is not already triggered before moving motors
            if(this->pin.get()) {
                gcode->stream->printf("ZProbe triggered before move, aborting command.\n");
                return;
            }

            if(is_delta) {
				if(gcode->has_letter('T')){
					if(!calibrate_delta_tower_geometry(gcode)){
                        gcode->stream->printf("Calibration failed to complete, probe not triggered\n");
						return;
                    }
                }
				else if(gcode->has_letter('A')||gcode->has_letter('B')||gcode->has_letter('C')){
					if(!fix_delta_tower_radius(gcode,0)) {
                        gcode->stream->printf("Calibration failed to complete, probe not triggered\n");
                        return;
                    }
                }
				else if(gcode->has_letter('X')||gcode->has_letter('Y')||gcode->has_letter('Z')){
					if(!fix_delta_tower_position(gcode,0)) {
                        gcode->stream->printf("Calibration failed to complete, probe not triggered\n");
                        return;
                    }				
				}
				else if(gcode->has_letter('P')){
					if(!assess_consistancy(gcode)) {
                        gcode->stream->printf("Calibration failed to complete, probe not triggered\n");
                        return;
                    }
                }
                else if(gcode->has_letter('E')){
					if(!calibrate_delta_endstops(gcode,0)) {
                        gcode->stream->printf("Calibration failed to complete, probe not triggered\n");
                        return;
                    }
                }
                if(gcode->has_letter('R')){
					if(!calibrate_delta_radius(gcode)) {
                        gcode->stream->printf("Calibration failed to complete, probe not triggered\n");
                        return;
                    }
                }
				if(gcode->has_letter('Q')){
					if(!assess_bed(gcode)) {
                        gcode->stream->printf("Calibration failed to complete, probe not triggered\n");
                        return;
                    }
                }
				
                gcode->stream->printf("Calibration complete, save settings with M500\n");

            } else {
                // TODO create Z height map for bed
                gcode->stream->printf("Not supported yet\n");
            }
        }


    } else if(gcode->has_m) {
        // M code processing here
        if(gcode->m == 119) {
            int c = this->pin.get();
            gcode->stream->printf(" Probe: %d", c);
            gcode->add_nl = true;
            gcode->mark_as_taken();

        } else if (gcode->m == 557) { // P0 Xxxx Yyyy sets probe points for G32
            // TODO will override the automatically calculated probe points for a delta, required for a cartesian

            gcode->mark_as_taken();
        }
    }
}

#define max(a,b) (((a) > (b)) ? (a) : (b))
// Called periodically to change the speed to match acceleration
uint32_t ZProbe::acceleration_tick(uint32_t dummy)
{
    if(!this->running) return(0); // nothing to do

    // foreach stepper that is moving
    for ( int c = X_AXIS; c <= Z_AXIS; c++ ) {
        if( !STEPPER[c]->is_moving() ) continue;

        uint32_t current_rate = STEPPER[c]->get_steps_per_second();
        uint32_t target_rate = int(floor(this->current_feedrate));

        if( current_rate < target_rate ) {
            uint32_t rate_increase = int(floor((THEKERNEL->planner->get_acceleration() / THEKERNEL->stepper->get_acceleration_ticks_per_second()) * STEPS_PER_MM(c)));
            current_rate = min( target_rate, current_rate + rate_increase );
        }
        if( current_rate > target_rate ) {
            current_rate = target_rate;
        }

        // steps per second
        STEPPER[c]->set_speed(max(current_rate, THEKERNEL->stepper->get_minimum_steps_per_second()));
    }

    return 0;
}

// issue a coordinated move directly to robot, and return when done
// Only move the coordinates that are passed in as not nan
void ZProbe::coordinated_move(float x, float y, float z, float feedrate, bool relative)
{
    char buf[32];
    char cmd[64];

    if(relative) strcpy(cmd, "G91 G0 ");
    else strcpy(cmd, "G0 ");

    if(!isnan(x)) {
        int n = snprintf(buf, sizeof(buf), " X%1.5f", x);
        strncat(cmd, buf, n);
    }
    if(!isnan(y)) {
        int n = snprintf(buf, sizeof(buf), " Y%1.5f", y);
        strncat(cmd, buf, n);
    }
    if(!isnan(z)) {
        int n = snprintf(buf, sizeof(buf), " Z%1.5f", z);
        strncat(cmd, buf, n);
    }

    // use specified feedrate (mm/sec)
    int n = snprintf(buf, sizeof(buf), " F%1.1f", feedrate * 60); // feed rate is converted to mm/min
    strncat(cmd, buf, n);
    if(relative) strcat(cmd, " G90");

    //THEKERNEL->streams->printf("DEBUG: move: %s\n", cmd);

    // send as a command line as may have multiple G codes in it
    struct SerialMessage message;
    message.message = cmd;
    message.stream = &(StreamOutput::NullStream);
    THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &message );
    THEKERNEL->conveyor->wait_for_empty_queue();
}

// issue home command
void ZProbe::home()
{
    Gcode gc("G28", &(StreamOutput::NullStream));
    THEKERNEL->call_event(ON_GCODE_RECEIVED, &gc);
}

bool ZProbe::set_trim(float x, float y, float z, StreamOutput *stream)
{
    float t[3]{x, y, z};
    bool ok= PublicData::set_value( endstops_checksum, trim_checksum, t);

    if (ok) {
        stream->printf("set trim to X:%f Y:%f Z:%f\n", x, y, z);
    } else {
        stream->printf("unable to set trim, is endstops enabled?\n");
    }

    return ok;
}

bool ZProbe::get_trim(float& x, float& y, float& z)
{
    void *returned_data;
    bool ok = PublicData::get_value( endstops_checksum, trim_checksum, &returned_data );

    if (ok) {
        float *trim = static_cast<float *>(returned_data);
        x= trim[0];
        y= trim[1];
        z= trim[2];
        return true;
    }
    return false;
}
