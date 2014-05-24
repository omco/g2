 /*
 * plan_line.c - acceleration managed line planning and motion execution
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2014 Alden S. Hart, Jr.
 * Copyright (c) 2012 - 2014 Rob Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "tinyg2.h"
#include "config.h"
#include "controller.h"
#include "canonical_machine.h"
#include "planner.h"
#include "stepper.h"
#include "report.h"
#include "util.h"

#ifdef __cplusplus
extern "C"{
#endif

// aline planner routines / feedhold planning

static void _plan_block_list(mpBuf_t *bf, uint8_t *mr_flag);
static void _calculate_trapezoid(mpBuf_t *bf);
//static float _get_jerk_value(const float Vi, const float Vt, const float L);
static float _get_target_length(const float Vi, const float Vt, const mpBuf_t *bf);
static float _get_target_velocity(const float Vi, const float L, const mpBuf_t *bf);
//static float _get_intersection_distance(const float Vi_squared, const float Vt_squared, const float L, const mpBuf_t *bf);
static float _get_junction_vmax(const float a_unit[], const float b_unit[]);
static void _reset_replannable_list(void);

/* Runtime-specific setters and getters
 *
 * mp_get_runtime_velocity() 		- returns current velocity (aggregate)
 * mp_get_runtime_machine_position() - returns current axis position in machine coordinates
 * mp_get_runtime_work_position() 	- returns current axis position in work coordinates
 *									  that were in effect at move planning time
 * mp_set_runtime_work_offset()
 * mp_zero_segment_velocity() 		- correct velocity in last segment for reporting purposes
 */

float mp_get_runtime_velocity(void) { return (mr.segment_velocity);}
float mp_get_runtime_absolute_position(uint8_t axis) { return (mr.position[axis]);}
float mp_get_runtime_work_position(uint8_t axis) { return (mr.position[axis] - mr.gm.work_offset[axis]);}
void mp_set_runtime_work_offset(float offset[]) { copy_vector(mr.gm.work_offset, offset);}
void mp_zero_segment_velocity() { mr.segment_velocity = 0;}

/*
 * mp_get_runtime_busy() - return TRUE if motion control busy (i.e. robot is moving)
 *
 *	Use this function to sync to the queue. If you wait until it returns
 *	FALSE you know the queue is empty and the motors have stopped.
 */

uint8_t mp_get_runtime_busy()
{
	if ((stepper_isbusy() == true) || (mr.move_state == MOVE_RUN)) return (true);
	return (false);
}

/* 
 * _calc_jerk_values()
 *
 *	This is a utility function to calculate jerk, recip_jerk, and cbrt_jerk,
 *  if the jerk value has changed.
 */

void _calc_jerk_values(mpBuf_t *bf)
{
//	if (fabs(bf->jerk - mm.prev_jerk) < JERK_MATCH_PRECISION) {	// can we re-use jerk terms?
//		bf->cbrt_jerk = mm.prev_cbrt_jerk;
//		bf->recip_jerk = mm.prev_recip_jerk;
//	} else {
		bf->cbrt_jerk = cbrt(bf->jerk);
		bf->recip_jerk = 1/bf->jerk;
		mm.prev_jerk = bf->jerk;
		mm.prev_cbrt_jerk = bf->cbrt_jerk;
		mm.prev_recip_jerk = bf->recip_jerk;
//	}
}

/**************************************************************************
 * mp_aline() - plan a line with acceleration / deceleration
 *
 *	This function uses constant jerk motion equations to plan acceleration 
 *	and deceleration. The jerk is the rate of change of acceleration; it's
 *	the 1st derivative of acceleration, and the 3rd derivative of position. 
 *	Jerk is a measure of impact to the machine. Controlling jerk smooths 
 *	transitions between moves and allows for faster feeds while controlling 
 *	machine oscillations and other undesirable side-effects.
 *
 * 	Note 1: All math is done in absolute coordinates using single precision
 *	floating point (float).
 *
 *	Note 2: Returning a status that is not STAT_OK means the endpoint is NOT
 *	advanced. So lines that are too short to move will accumulate and get
 *	executed once the accumulated error exceeds the minimums
 */

/*
#define __NEW_JERK
static float _get_relative_length(const float Vi, const float Vt, const float jerk)
{
	return (fabs(Vi-Vt) * sqrt(fabs(Vi-Vt) / jerk));
}
*/

stat_t mp_aline(const GCodeState_t *gm_in)
{
	mpBuf_t *bf; 						// current move pointer
	float exact_stop = 0;				// preset this value OFF
	float junction_velocity;

	// exit out if the move has zero movement. At all.
	if (vector_equal(mm.position, gm_in->target)) return (STAT_OK);

	// trap short lines
	//	if (length < MIN_LENGTH_MOVE) { return (STAT_MINIMUM_LENGTH_MOVE);}
//	if (gm_in->move_time < MIN_TIME_MOVE) {
//		printf("ALINE() line%lu %f\n", gm_in->linenum, (double)gm_in->move_time);
//		return (STAT_MINIMUM_TIME_MOVE);
//	}

	// get a cleared buffer and setup move variables
	if ((bf = mp_get_write_buffer()) == NULL) return(cm_hard_alarm(STAT_BUFFER_FULL_FATAL)); // never supposed to fail
	bf->length = get_axis_vector_length(gm_in->target, mm.position);// compute the length
	bf->bf_func = mp_exec_aline;									// register the callback to the exec function
	memcpy(&bf->gm, gm_in, sizeof(GCodeState_t));					// copy model state into planner buffer

#ifndef __NEW_JERK
	// compute both the unit vector and the jerk term in the same pass for efficiency
	float diff = bf->gm.target[AXIS_X] - mm.position[AXIS_X];
	if (fp_NOT_ZERO(diff)) {
		bf->unit[AXIS_X] = diff / bf->length;
		bf->jerk = square(bf->unit[AXIS_X] * cm.a[AXIS_X].jerk_max);
	}
	if (fp_NOT_ZERO(diff = bf->gm.target[AXIS_Y] - mm.position[AXIS_Y])) {
		bf->unit[AXIS_Y] = diff / bf->length;
		bf->jerk += square(bf->unit[AXIS_Y] * cm.a[AXIS_Y].jerk_max);
	}
	if (fp_NOT_ZERO(diff = bf->gm.target[AXIS_Z] - mm.position[AXIS_Z])) {
		bf->unit[AXIS_Z] = diff / bf->length;
		bf->jerk += square(bf->unit[AXIS_Z] * cm.a[AXIS_Z].jerk_max);
	}
	if (fp_NOT_ZERO(diff = bf->gm.target[AXIS_A] - mm.position[AXIS_A])) {
		bf->unit[AXIS_A] = diff / bf->length;
		bf->jerk += square(bf->unit[AXIS_A] * cm.a[AXIS_A].jerk_max);
	}
	if (fp_NOT_ZERO(diff = bf->gm.target[AXIS_B] - mm.position[AXIS_B])) {
		bf->unit[AXIS_B] = diff / bf->length;
		bf->jerk += square(bf->unit[AXIS_B] * cm.a[AXIS_B].jerk_max);
	}
	if (fp_NOT_ZERO(diff = bf->gm.target[AXIS_C] - mm.position[AXIS_C])) {
		bf->unit[AXIS_C] = diff / bf->length;
		bf->jerk += square(bf->unit[AXIS_C] * cm.a[AXIS_C].jerk_max);
	}
	bf->jerk = sqrt(bf->jerk) * JERK_MULTIPLIER;

#else	// compute unit vector	
	float diff;
 	for (uint8_t axis=AXIS_X; axis<AXIS_C; axis++) {
		if (fp_NOT_ZERO(diff = bf->gm.target[axis] - mm.position[axis])) {
			bf->unit[axis] = diff / bf->length;
		}
	}

	// find the dominant jerk term
	float axis_relative_length = 0;
	float longest_axis_length = 0;
	junction_velocity = _get_junction_vmax(bf->pv->unit, bf->unit);			// initial velocity
	bf->cruise_vmax = bf->length / bf->gm.move_time;						// target velocity (requested, if not achieved)

	// alternate #1 - relative length computed via subroutine
	for (uint8_t axis=AXIS_X; axis<AXIS_C; axis++) {
		if (fp_NOT_ZERO(bf->unit[axis])) {
			axis_relative_length = _get_relative_length( junction_velocity * bf->unit[axis],
														 bf->cruise_vmax* bf->unit[axis], 
														 cm.a[axis].jerk_max * JERK_MULTIPLIER);
			if (axis_relative_length > longest_axis_length) {
				longest_axis_length = axis_relative_length;
				bf->jerk = cm.a[axis].jerk_max * JERK_MULTIPLIER;
			}
		}
	}
/*
	// alternate #2 - relative length computed inline
	for (uint8_t axis=AXIS_X; axis<AXIS_C; axis++) {
		if (fp_NOT_ZERO(bf->unit[axis])) {
			axis_relative_length = (fabs(junction_velocity * bf->unit[axis] - bf->cruise_vmax * bf->unit[axis]) *
									sqrt(fabs(junction_velocity * bf->unit[axis] - bf->cruise_vmax * bf->unit[axis]) /
									cm.a[axis].jerk_max * JERK_MULTIPLIER));
			if (axis_relative_length > longest_axis_length) {
				longest_axis_length = axis_relative_length;
				bf->jerk = cm.a[axis].jerk_max * JERK_MULTIPLIER;
			}
		}
	}

	// alternate #3 - relative length computed inline as an abbreviated expression
	for (uint8_t axis=AXIS_X; axis<AXIS_C; axis++) {
		if (fp_NOT_ZERO(bf->unit[axis])) {
			axis_relative_length = fabs(bf->unit[axis] * cm.a[axis].jerk_max);
			if (axis_relative_length > longest_axis_length) {
				longest_axis_length = axis_relative_length;
				bf->jerk = cm.a[axis].jerk_max * JERK_MULTIPLIER;
			}
		}
	}
*/
#endif // __NEW_JERK

    _calc_jerk_values(bf);

	// finish up the current block variables
	if (cm_get_path_control(MODEL) != PATH_EXACT_STOP) { 	// exact stop cases already zeroed
		bf->replannable = true;
		exact_stop = 8675309;								// an arbitrarily large floating point number
	}
	bf->cruise_vmax = bf->length / bf->gm.move_time;		// target velocity requested
	junction_velocity = _get_junction_vmax(bf->pv->unit, bf->unit);
	bf->entry_vmax = min3(bf->cruise_vmax, junction_velocity, exact_stop);
	bf->delta_vmax = _get_target_velocity(0, bf->length, bf);
	bf->exit_vmax = min3(bf->cruise_vmax, (bf->entry_vmax + bf->delta_vmax), exact_stop);
	bf->braking_velocity = bf->delta_vmax;

	uint8_t mr_flag = false;

	// Note: these next lines must remain in exact order. Position must update before committing the buffer.
	_plan_block_list(bf, &mr_flag);				// replan block list
	copy_vector(mm.position, bf->gm.target);	// set the planner position
	mp_commit_write_buffer(MOVE_TYPE_ALINE); 	// commit current block (must follow the position update)
	return (STAT_OK);
}

/***** ALINE HELPERS *****
 * _plan_block_list()
 * _calculate_trapezoid()
 * _get_target_length()
 * _get_target_velocity()
 * _get_junction_vmax()
 * _reset_replannable_list()
 */

/* _plan_block_list() - plans the entire block list
 *
 *	The block list is the circular buffer of planner buffers (bf's). The block currently 
 *	being planned is the "bf" block. The "first block" is the next block to execute; 
 *	queued immediately behind the currently executing block, aka the "running" block. 
 *	In some cases there is no first block because the list is empty or there is only 
 *	one block and it is already running. 
 *
 *	If blocks following the first block are already optimally planned (non replannable)
 *	the first block that is not optimally planned becomes the effective first block.
 *
 *	_plan_block_list() plans all blocks between and including the (effective) first block 
 *	and the bf. It sets entry, exit and cruise V's from Vmax's then calls trapezoid generation. 
 *
 *	Variables that must be provided in the mpBuffers that will be processed:
 *
 *	  bf (function arg)		- end of block list (last block in time)
 *	  bf->replannable		- start of block list set by last FALSE value [Note 1]
 *	  bf->move_type			- typically MOVE_TYPE_ALINE. Other move_types should be set to 
 *							  length=0, entry_vmax=0 and exit_vmax=0 and are treated
 *							  as a momentary stop (plan to zero and from zero).
 *
 *	  bf->length			- provides block length
 *	  bf->entry_vmax		- used during forward planning to set entry velocity
 *	  bf->cruise_vmax		- used during forward planning to set cruise velocity
 *	  bf->exit_vmax			- used during forward planning to set exit velocity
 *	  bf->delta_vmax		- used during forward planning to set exit velocity
 *
 *	  bf->recip_jerk		- used during trapezoid generation
 *	  bf->cbrt_jerk			- used during trapezoid generation
 *
 *	Variables that will be set during processing:
 *
 *	  bf->replannable		- set if the block becomes optimally planned
 *
 *	  bf->braking_velocity	- set during backward planning
 *	  bf->entry_velocity	- set during forward planning
 *	  bf->cruise_velocity	- set during forward planning
 *	  bf->exit_velocity		- set during forward planning
 *
 *	  bf->head_length		- set during trapezoid generation
 *	  bf->body_length		- set during trapezoid generation
 *	  bf->tail_length		- set during trapezoid generation
 *
 *	Variables that are ignored but here's what you would expect them to be:
 *	  bf->move_state		- NEW for all blocks but the earliest
 *	  bf->target[]			- block target position
 *	  bf->unit[]			- block unit vector
 *	  bf->time				- gets set later
 *	  bf->jerk				- source of the other jerk variables. Used in mr.
 */
/* Notes:
 *	[1]	Whether or not a block is planned is controlled by the bf->replannable 
 *		setting (set TRUE if it should be). Replan flags are checked during the 
 *		backwards pass and prune the replan list to include only the the latest 
 *		blocks that require planning
 *
 *		In normal operation the first block (currently running block) is not 
 *		replanned, but may be for feedholds and feed overrides. In these cases 
 *		the prep routines modify the contents of the mr buffer and re-shuffle 
 *		the block list, re-enlisting the current bf buffer with new parameters.
 *		These routines also set all blocks in the list to be replannable so the 
 *		list can be recomputed regardless of exact stops and previous replanning 
 *		optimizations.
 *
 *	[2] The mr_flag is used to tell replan to account for mr buffer's exit velocity (Vx)
 *		mr's Vx is always found in the provided bf buffer. Used to replan feedholds
 */
static void _plan_block_list(mpBuf_t *bf, uint8_t *mr_flag)
{
	mpBuf_t *bp = bf;

	// Backward planning pass. Find first block and update the braking velocities.
	// At the end *bp points to the buffer before the first block.
	while ((bp = mp_get_prev_buffer(bp)) != bf) {
		if (bp->replannable == false) { break; }
		bp->braking_velocity = min(bp->nx->entry_vmax, bp->nx->braking_velocity) + bp->delta_vmax;
	}

	// forward planning pass - recomputes trapezoids in the list from the first block to the bf block.
	bp = mp_get_next_buffer(bp);
    while (bp != bf) {
        if ((bp->pv == bf) || (*mr_flag == true))  {
            bp->entry_velocity = bp->entry_vmax;		// first block in the list
            *mr_flag = false;
        } else {
            bp->entry_velocity = bp->pv->exit_velocity;	// other blocks in the list
        }
        bp->cruise_velocity = bp->cruise_vmax;
        bp->exit_velocity = min4( bp->exit_vmax, 
                                  bp->nx->entry_vmax,
                                  bp->nx->braking_velocity, 
                                 (bp->entry_velocity + bp->delta_vmax) );

        float old_entry_velocity = bp->entry_velocity;
        _calculate_trapezoid(bp);
//        _calc_jerk_values(bp);

        // If we changed the entry velocity, we need to account for it...
//        if (!fp_EQ(bp->entry_velocity, old_entry_velocity)) {
//            bp->entry_vmax = bp->entry_velocity;
//            bp = mp_get_prev_buffer(bp);
//            continue;
//        }

        // test for optimally planned trapezoids - only need to check various exit conditions
        if ( ( (fp_EQ(bp->exit_velocity, bp->exit_vmax)) ||
               (fp_EQ(bp->exit_velocity, bp->nx->entry_vmax)) )  ||
             ( (bp->pv->replannable == false) &&
               (fp_EQ(bp->exit_velocity, (bp->entry_velocity + bp->delta_vmax))) ) ) {
            bp->replannable = false;
        }
        bp = mp_get_next_buffer(bp);
    }

	// finish up the last block move
	bp->entry_velocity = bp->pv->exit_velocity;
	bp->cruise_velocity = bp->cruise_vmax;
	bp->exit_velocity = 0;
	_calculate_trapezoid(bp);
//    _calc_jerk_values(bp);
}

/*
 *	_reset_replannable_list() - resets all blocks in the planning list to be replannable
 */	
static void _reset_replannable_list()
{
	mpBuf_t *bf = mp_get_first_buffer();
	if (bf == NULL) { return;}
	mpBuf_t *bp = bf;
	do {
		bp->replannable = true;
	} while (((bp = mp_get_next_buffer(bp)) != bf) && (bp->move_state != MOVE_OFF));
}

/*
 * _calculate_trapezoid() - calculate trapezoid parameters
 *
 *	This rather brute-force and long-ish function sets section lengths and velocities 
 *	based on the line length and velocities requested. It modifies the incoming 
 *	bf buffer and returns accurate head, body and tail lengths, and accurate or 
 *	reasonably approximate velocities. We care about accuracy on lengths, less 
 *	so for velocity (as long as velocity err's on the side of too slow). 
 *
 *	Note: We need the velocities to be set even for zero-length sections 
 *	(Note: sections, not moves) so we can compute entry and exits for adjacent sections.
 *
 *	Inputs used are:
 *	  bf->length			- actual block length (must remain accurate)
 *	  bf->entry_velocity	- requested Ve
 *	  bf->cruise_velocity	- requested Vt
 *	  bf->exit_velocity		- requested Vx
 *	  bf->cruise_vmax		- used in some comparisons
 *
 *	Variables that may be set/updated are:
 *    bf->entry_velocity	- requested Ve
 *	  bf->cruise_velocity	- requested Vt
 *	  bf->exit_velocity		- requested Vx
 *	  bf->head_length		- bf->length allocated to head
 *	  bf->body_length		- bf->length allocated to body
 *	  bf->tail_length		- bf->length allocated to tail
 *
 *	Note: The following conditions must be met on entry: 
 *		bf->length must be non-zero (filter these out upstream)
 *		bf->entry_velocity <= bf->cruise_velocity >= bf->exit_velocity
 */
/*	Classes of moves:
 *
 *	  Requested-Fit - The move has sufficient length to achieve the target velocity
 *		(cruise velocity). I.e: it will accommodate the acceleration / deceleration 
 *		profile in the given length.
 *
 *	  Rate-Limited-Fit - The move does not have sufficient length to achieve target 
 *		velocity. In this case the cruise velocity will be set lower than the requested 
 *		velocity (incoming bf->cruise_velocity). The entry and exit velocities are satisfied.
 *
 *	  Degraded-Fit - The move does not have sufficient length to transition from
 *		the entry velocity to the exit velocity in the available length. These 
 *		velocities are not negotiable, so a degraded solution is found.
 *
 *	  	In worst cases the move cannot be executed as the required execution time is 
 *		less than the minimum segment time. The first degradation is to reduce the 
 *		move to a body-only segment with an average velocity. If that still doesn't 
 *		fit then the move velocity is reduced so it fits into a minimum segment.
 *		This will reduce the velocities in that region of the planner buffer as the 
 *		moves are replanned to that worst-case move.
 *
 *	Various cases handled (H=head, B=body, T=tail)
 *
 *	  Requested-Fit cases
 *	  	HBT	Ve<Vt>Vx	sufficient length exists for all parts (corner case: HBT')
 *	  	HB	Ve<Vt=Vx	head accelerates to cruise - exits at full speed (corner case: H')
 *	  	BT	Ve=Vt>Vx	enter at full speed and decelerate (corner case: T')
 *	  	HT	Ve & Vx		perfect fit HT (very rare). May be symmetric or asymmetric
 *	  	H	Ve<Vx		perfect fit H (common, results from planning)
 *	  	T	Ve>Vx		perfect fit T (common, results from planning)
 *	  	B	Ve=Vt=Vx	Velocities are close to each other and within matching tolerance
 *
 *	  Rate-Limited cases - Ve and Vx can be satisfied but Vt cannot
 *	  	HT	(Ve=Vx)<Vt	symmetric case. Split the length and compute Vt.
 *	  	HT'	(Ve!=Vx)<Vt	asymmetric case. Find H and T by successive approximation.
 *		HBT'			body length < min body length - treated as an HT case
 *		H'				body length < min body length - subsume body into head length
 *		T'				body length < min body length - subsume body into tail length
 *
 *	  Degraded fit cases - line is too short to satisfy both Ve and Vx
 *	    H"	Ve<Vx		Ve is degraded (velocity step). Vx is met
 *	  	T"	Ve>Vx		Ve is degraded (velocity step). Vx is met
 *	  	B"	<short>		line is very short but drawable; is treated as a body only
 *		F	<too short>	force fit: This block is slowed down until it can be executed
 */
/*	NOTE: The order of the cases/tests in the code is pretty important. Start with the 
 *	  shortest cases first and work up. Not only does this simplfy the order of the tests,
 *	  but it reduces execution time when you need it most - when tons of pathologically
 *	  short Gcode blocks are being thrown at you.
 */

// The minimum lengths are dynamic and depend on the velocity
// These expressions evaluate to the minimum lengths for the current velocity settings
// Note: The head and tail lengths are 2 minimum segments, the body is 1 min segment
#define MIN_HEAD_LENGTH (MIN_SEGMENT_TIME_PLUS_MARGIN * (bf->cruise_velocity + bf->entry_velocity))
#define MIN_TAIL_LENGTH (MIN_SEGMENT_TIME_PLUS_MARGIN * (bf->cruise_velocity + bf->exit_velocity))
#define MIN_BODY_LENGTH (MIN_SEGMENT_TIME_PLUS_MARGIN * bf->cruise_velocity)

static void _calculate_trapezoid(mpBuf_t *bf) 
{
    /*********************************
     *********************************
     **                             **
     **      THE FIRST RULE OF      **
     **    _calculate_trapezoid():  **
     **        DON'T CHANGE         **
     **         bf->length          **
     **                             **
     *********************************
     *********************************/

	// F case: Block is too short to execute. 
	// Force block into a single segment body with limited velocities

	// if length < segment time * average velocity
	float average_velocity = (bf->entry_velocity + bf->cruise_velocity) / 2;
	if (bf->length < (MIN_SEGMENT_TIME_PLUS_MARGIN * average_velocity)) {
		bf->entry_velocity = bf->length / MIN_SEGMENT_TIME_PLUS_MARGIN;
		bf->cruise_velocity = bf->entry_velocity;
		bf->exit_velocity = bf->entry_velocity;
		bf->body_length = bf->length;
		bf->head_length = 0;
		bf->tail_length = 0;
        // We have invalidated jerk, but don't use it...
		return;
	}

	// B" case: Short line, body only. See if the block fits into a single segment

	if (bf->length <= (NOM_SEGMENT_TIME * average_velocity)) {
		bf->entry_velocity = bf->length / NOM_SEGMENT_TIME;
		bf->cruise_velocity = bf->entry_velocity;
		bf->exit_velocity = bf->entry_velocity;
		bf->body_length = bf->length;
		bf->head_length = 0;
		bf->tail_length = 0;
        // We have invalidated jerk, but don't use it...
		return;
	}

	// B case:  Velocities all match (or close enough)
	//			This occurs frequently in normal gcode files with lots of short lines

//	if (((bf->cruise_velocity - bf->entry_velocity) < TRAPEZOID_VELOCITY_TOLERANCE) && 
//		((bf->cruise_velocity - bf->exit_velocity) < TRAPEZOID_VELOCITY_TOLERANCE)) { 
//		bf->body_length = bf->length;
//		bf->head_length = 0;
//		bf->tail_length = 0;
//		return;
//	}

	// Head-only and tail-only short-line cases
	//	 H" and T" degraded-fit cases
	//	 H' and T' requested-fit cases where the body residual is less than MIN_BODY_LENGTH
	
	bf->body_length = 0;

    // if bf->entry_velocity == bf->exit_velocity, we'll get a zero minimum_length
    if (fp_EQ(bf->entry_velocity, bf->exit_velocity)) {
        // head_length == tail_length, only calculate once
        bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
        // If the speed change is too little, the head and tail will be too short
        if (bf->head_length < MIN_HEAD_LENGTH) {
            bf->head_length = 0;
        }
        bf->tail_length = bf->head_length;
    } else {
        float minimum_length = _get_target_length(bf->entry_velocity, bf->exit_velocity, bf);
        if (bf->length <= (minimum_length + MIN_BODY_LENGTH)) {	// head-only & tail-only cases

            if (bf->entry_velocity > bf->exit_velocity)	{		// tail-only cases (short decelerations)
                if (bf->length < minimum_length) { 				// T" (degraded case)
                    // RAISE the EXIT velocity to take at least two segments worth of time to decelerate.

                    /* NOTE: If we are assuming we're going to recalculate the jerk, *and* we are going to not
                     *   change the length, then we actually want to make the two speeds *closer together*.
                     *   This is because the distance traveled (L) is actually the area of a right triangle formed
                     *   by the ΔV (velocity change) on one side and T (the time of the move) on the bottom:
                     *   
                     *        |\      Area of the triangle = L (distance traveled)
                     *     ΔV | \                  (V*T)/2 = L  →  VT = 2L  →  V = 2L/T
                     *         --
                     *         T
                     *
                     *   Normally we are bounded by the jerk value, but as long as we only *lower* ΔV while not
                     *   changing the distance traveled (area of the triangle), then we are lowering the jerk value,
                     *   while making the move take more time, which is what we want.
                     *
                     *   Additionally, since we want the time to be twice MIN_SEGMENT_TIME_PLUS_MARGIN, we will
                     *   replace T = (t*2), to get (V*(t*2))/2 = L  →  Vt = L  →  V = L/t
                     */

//                    bf->entry_velocity = min(bf->exit_velocity + (bf->length / MIN_SEGMENT_TIME_PLUS_MARGIN),
//                                             _get_target_velocity(bf->exit_velocity, bf->length, bf));

//                    bf->entry_velocity = bf->exit_velocity + (bf->length / MIN_SEGMENT_TIME_PLUS_MARGIN);
                    bf->exit_velocity = bf->entry_velocity - (bf->length / MIN_SEGMENT_TIME_PLUS_MARGIN);
                }
                bf->cruise_velocity = bf->entry_velocity;
                bf->tail_length = bf->length;
                bf->head_length = 0;
                return;
            }

            if (bf->entry_velocity < bf->exit_velocity)	{		// head-only cases (short accelerations)
                if (bf->length < minimum_length) { 				// H" (degraded case)
                    // Lower the exit velocity to take at least two segments worth of time to decelerate.
                    // See monstrous note from above.

//                    bf->exit_velocity = min(bf->entry_velocity + (bf->length / MIN_SEGMENT_TIME_PLUS_MARGIN),
//                                            _get_target_velocity(bf->entry_velocity, bf->length, bf));
                    bf->exit_velocity = bf->entry_velocity + (bf->length / MIN_SEGMENT_TIME_PLUS_MARGIN);
                }
                bf->cruise_velocity = bf->exit_velocity;
                bf->head_length = bf->length;
                bf->tail_length = 0;
                return;
            }
        }

        // Set head and tail lengths for evaluating the next cases
        bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
        bf->tail_length = _get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
        if (bf->head_length < MIN_HEAD_LENGTH) { bf->head_length = 0;}
        if (bf->tail_length < MIN_TAIL_LENGTH) { bf->tail_length = 0;}
    }

	// Rate-limited HT and HT' cases
	if (bf->length < (bf->head_length + bf->tail_length)) { // it's rate limited

		// Symmetric rate-limited case (HT)
//		if (fabs(bf->entry_velocity - bf->exit_velocity) < TRAPEZOID_VELOCITY_TOLERANCE) {
		if (fp_EQ(bf->entry_velocity, bf->exit_velocity)) {
			bf->head_length = bf->length/2;
			bf->tail_length = bf->head_length;
			bf->cruise_velocity = min(bf->cruise_vmax, _get_target_velocity(bf->entry_velocity, bf->head_length, bf));

			if (bf->head_length < MIN_HEAD_LENGTH) {
				// Convert this to a body-only move
				bf->body_length = bf->length;
				bf->head_length = 0;
				bf->tail_length = 0;

				// Average the entry speed and computed best cruise-speed
				bf->cruise_velocity = (bf->entry_velocity + bf->cruise_velocity)/2;
				bf->entry_velocity = bf->cruise_velocity;
				bf->exit_velocity = bf->cruise_velocity;
            }
            return;
		}

		// Asymmetric HT' rate-limited case. This is relatively expensive but it's not called very often
		// iteration trap: uint8_t i=0;
		// iteration trap: if (++i > TRAPEZOID_ITERATION_MAX) { fprintf_P(stderr,PSTR("_calculate_trapezoid() failed to converge"));}

		float computed_velocity = bf->cruise_vmax;
        float max_velocity = max(bf->entry_velocity, bf->exit_velocity);
		do {
			bf->cruise_velocity = computed_velocity;	// initialize from previous iteration 
			bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
			bf->tail_length = _get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
#if 1
            float zero_test = (bf->head_length + bf->tail_length) - bf->length;
            if (fp_ZERO(zero_test))
                break;
            // V_d(x) = (sqrt(abs(x - V_s)) + (x - V_s) (x + V_s) / (2abs(x - V_s)^(3 / 2)) + sqrt(abs(x - V_e)) + (x - V_e) (x + V_e) / (2abs(x - V_e)^(3 / 2))) / sqrt(J)
            float abs_x_minus_Vs = fabs(computed_velocity - bf->entry_velocity);
            float x_plus_Vs = (computed_velocity + bf->entry_velocity);
            float abs_x_minus_Ve = fabs(computed_velocity - bf->exit_velocity);
            float x_plus_Ve = (computed_velocity + bf->exit_velocity);

            // V_d(x) = (sqrt(abs_x_minus_Vs) + abs_x_minus_Vs * x_plus_Vs / (2 * abs_x_minus_Vs^(3 / 2)) + sqrt(abs_x_minus_Ve) + abs_x_minus_Ve x_plus_Ve / (2 * abs_x_minus_Ve^(3 / 2))) / sqrt(J)

            float derivative = (sqrt(abs_x_minus_Vs) + abs_x_minus_Vs * x_plus_Vs / (2 * pow(abs_x_minus_Vs, 3 / 2)) + sqrt(abs_x_minus_Ve) + abs_x_minus_Ve * x_plus_Ve / (2 * pow(abs_x_minus_Ve, 3 / 2))) / sqrt(bf->jerk);

            float test_velocity = computed_velocity - zero_test/derivative;
            if (test_velocity < max_velocity) {
                test_velocity = 2*max_velocity - test_velocity;
            }
            computed_velocity = test_velocity;
		} while (!fp_ZERO(bf->cruise_velocity - computed_velocity));
#else
			if (bf->head_length > bf->tail_length) {
				bf->head_length = (bf->head_length / (bf->head_length + bf->tail_length)) * bf->length;
				computed_velocity = _get_target_velocity(bf->entry_velocity, bf->head_length, bf);
			} else {
				bf->tail_length = (bf->tail_length / (bf->head_length + bf->tail_length)) * bf->length;
				computed_velocity = _get_target_velocity(bf->exit_velocity, bf->tail_length, bf);
			}
			// insert iteration trap here if needed
		} while ((fabs(bf->cruise_velocity - computed_velocity) / computed_velocity) > TRAPEZOID_ITERATION_ERROR_PERCENT);
#endif

		// set velocity and clean up any parts that are too short
		bf->cruise_velocity = computed_velocity;
		bf->head_length = _get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
		bf->tail_length = bf->length - bf->head_length;
		if (bf->head_length < MIN_HEAD_LENGTH) {
			bf->tail_length = bf->length;			// adjust the move to be all tail...
			bf->head_length = 0;
		}
		if (bf->tail_length < MIN_TAIL_LENGTH) {
			bf->head_length = bf->length;			//...or all head
			bf->tail_length = 0;
		}
		return;
	}

	// Requested-fit cases: remaining of: HBT, HB, BT, BT, H, T, B, cases
	bf->body_length = bf->length - bf->head_length - bf->tail_length;

	// If a non-zero body is < minimum length distribute it to the head and/or tail
	// This will force us to recompute the jerk, but preserve correct distance, which is more important.
	if ((bf->body_length < MIN_BODY_LENGTH) && (fp_NOT_ZERO(bf->body_length))) {
		if (fp_NOT_ZERO(bf->head_length)) {
			if (fp_NOT_ZERO(bf->tail_length)) {			// HBT reduces to HT
				bf->head_length = bf->length/2;
				bf->tail_length = bf->head_length;
			} else {									// HB reduces to H
				bf->head_length = bf->length;
				bf->tail_length = 0;
			}
		} else {										// BT reduces to T
			bf->tail_length = bf->length;
            bf->head_length = 0;
		}
		bf->body_length = 0;

	// If the body is a standalone make the cruise velocity match the entry velocity
	// This removes a potential velocity discontinuity at the expense of top speed
	} else if ((fp_ZERO(bf->head_length)) && (fp_ZERO(bf->tail_length))) {
        // WARNING: Edge case where entry_velocity is zero is a crash-situation.
        // Also, we want to have an intelligent speed set here. Let's use the maximum
        // speed we can attain in half the body length, and average it.
        // We shpuldn't be here if cruise_velocity == 0.
        bf->cruise_velocity = min(
                                  _get_target_velocity(bf->entry_velocity, bf->body_length/2, bf),
                                  bf->cruise_velocity
                                  );
//		float new_cruise = (bf->entry_velocity+bf->cruise_velocity)/2.0;
        bf->cruise_velocity = (bf->entry_velocity+bf->cruise_velocity)/2.0;
	}
}

/*	
 * _get_target_length()	  - derive accel/decel length from delta V and jerk
 * _get_target_velocity() - derive velocity achievable from delta V and length
 *
 *	This set of functions returns the fourth thing knowing the other three.
 *	
 * 	  Jm = the given maximum jerk
 *	  T  = time of the entire move
 *	  T  = 2*sqrt((Vt-Vi)/Jm)
 *	  As = The acceleration at inflection point between convex and concave portions of the S-curve.
 *	  As = (Jm*T)/2
 *    Ar = ramp acceleration
 *	  Ar = As/2 = (Jm*T)/4
 *	
 *	Assumes Vt, Vi and L are positive or zero
 *	Cannot assume Vt>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *	necessitating the introduction of fabs()

 *	_get_target_length() is a convenient function for determining the 
 *	optimal_length (L) of a line given the inital velocity (Vi), 
 *	target velocity (Vt) and maximum jerk (Jm).
 *
 *	The length (distance) equation is derived from: 
 *
 *	 a)	L = (Vt-Vi) * T - (Ar*T^2)/2	... which becomes b) with substitutions for Ar and T
 *	 b) L = (Vt-Vi) * 2*sqrt((Vt-Vi)/Jm) - (2*sqrt((Vt-Vi)/Jm) * (Vt-Vi))/2
 *	 c)	L = (Vt-Vi)^(3/2) / sqrt(Jm)	...is an alternate form of b) (see Wolfram Alpha)
 *	 c')L = (Vt-Vi) * sqrt((Vt-Vi)/Jm) ... second alternate form; requires Vt >= Vi
 *
 *	 Notes: Ar = (Jm*T)/4					Ar is ramp acceleration
 *			T  = 2*sqrt((Vt-Vi)/Jm)			T is time
 *			Assumes Vt, Vi and L are positive or zero
 *			Cannot assume Vt>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *			  necessitating the introduction of fabs()
 *
 * 	_get_target_velocity() is a convenient function for determining Vt target 
 *	velocity for a given the initial velocity (Vi), length (L), and maximum jerk (Jm).
 *	Equation d) is b) solved for Vt. Equation e) is c) solved for Vt. Use e) (obviously)
 *
 *	 d)	Vt = (sqrt(L)*(L/sqrt(1/Jm))^(1/6)+(1/Jm)^(1/4)*Vi)/(1/Jm)^(1/4)
 *	 e)	Vt = L^(2/3) * Jm^(1/3) + Vi
 *
 *  FYI: Here's an expression that returns the jerk for a given deltaV and L:
 * 	return(cube(deltaV / (pow(L, 0.66666666))));
 */

//static float _get_jerk_value(const float Vi, const float Vt, const float L)
//{
//    return fabs( ((Vt-Vi) * pow((Vi+Vt), 2)) / pow(L, 2) ) ;
//}

static float _get_target_length(const float Vi, const float Vt, const mpBuf_t *bf)
{
    return (Vi + Vt) * sqrt(fabs(Vt - Vi) * bf->recip_jerk);
//	return fabs(Vi-Vt) * sqrt(fabs(Vi-Vt) * bf->recip_jerk);
}

static float _get_target_velocity(const float Vi, const float L, const mpBuf_t *bf)
{
    // We start with a reasonable estimate...
    float estimate = pow(L, 0.66666666) * bf->cbrt_jerk + Vi;

    /* Now we'll do some Newton-Raphson iterations to narrow it down.
     * We need a formula that includes know variables except the one we want to find,
     * and has a root [Z(x) = 0] at the value (x) we are looking for.
     *
     *      Z(x) = zero at x -- we calculate the value from the knowns and the estimate
     *             (see below) and then subtract the known value to get zero (root) if
     *             x is the correct value.
     *      x    = estimated final velocity, or Ve
     *      Vi   = initial velocity (known)
     *      J    = jerk (known)
     *      L    = length (know)
     *
     * There are (at least) two such functions we can use:
     *      L from J, Vi, and Ve
     *      L = sqrt((Ve - Vi) / J) (Vi + Ve)
     *   Replacing Ve with x, and subtracting the known L:
     *      0 = sqrt((x - Vi) / J) (Vi + x) - L
     *      Z(x) = sqrt((x - Vi) / J) (Vi + x) - L
     *
     *  OR
     *
     *      J from L, Vi, and Ve
     *      J = ((Ve - Vi) (Vi + Ve)²) / L²
     *  Replacing Ve with x, and subtracting the known J:
     *      0 = ((x - Vi) (Vi + x)²) / L² - J
     *      Z(x) = ((x - Vi) (Vi + x)²) / L² - J
     *
     *  L doesn't resolve to the value very quickly (it graphs near-vertical).
     *  So, we'll use J, which resolves in < 10 iterations, often in only two or three
     *  with a good estimate.
     *
     *  In order to do a Newton-Raphson iteration, we need the derivative. Here they are
     *  for both the (unused) L and the (used) J formulas above:
     *
     *  J > 0, Vi > 0, x > 0
     *  SqrtDeltaJ = sqrt((x-Vi) * J)
     *  SqrtDeltaOverJ = sqrt((x-Vi) / J)
     *  L'(x) = SqrtDeltaOverJ + (Vi + x) / (2*J) + (Vi + x) / (2*SqrtDeltaJ)
     *
     *  J'(x) = (2*Vi*x - Vi² + 3*x²) / L²
     *
     *
     */

    float L_squared = pow(L,2);
    float Vi_squared = pow(Vi,2);

    float previous_estimate = 0;
    int8_t i = 10; // Only allow it to iterate 10 times
    do {
        previous_estimate = estimate;
        float J_z = ((estimate - Vi)*pow((Vi + estimate),2)) / L_squared - bf->jerk;
        float J_d = (2*Vi*estimate - Vi_squared + 3*pow(estimate,2)) / L_squared;
        estimate = estimate - J_z/J_d;
    } while (i-- != 0 && !fp_EQ(previous_estimate, estimate));

    return estimate;
}

// NOTE: ALTERNATE FORMULATION OF ABOVE...

/*	
 * _get_target_length2()   - derive accel/decel length from delta V and jerk
 * _get_target_velocity2() - derive velocity achievable from initial V, length and jerk
 *
 *	This set of functions returns the fourth thing knowing the other three.
 *	
 * 	  Jm = the given maximum jerk
 *	  T  = time of the entire move
 *	  T  = 2*sqrt((Vt-Vi)/Jm)
 *	  As = The acceleration at inflection point between convex and concave portions of the S-curve.
 *	  As = (Jm*T)/2
 *    Ar = ramp acceleration
 *	  Ar = As/2 = (Jm*T)/4
 *	
 *	Assumes Vt, Vi and L are positive or zero
 *	Cannot assume Vt>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *	necessitating the introduction of fabs()
 *
 *	_get_target_length() is a convenient function for determining the optimal_length (L) 
 *	of a line given the inital velocity (Vi), target velocity (Vt) and maximum jerk (Jm).
 *
 *	The length (distance) equation is derived from: 
 *
 *	 a) L = Vi * Td + (Ar*Td^2)/2		... which becomes b) with substitutions for Ar and T
 *	 b) L = 2 * (Vi*sqrt((Vt-Vi)/Jm) + sqrt((Vt-Vi)/Jm)/2 * (Vt-Vi))
 *	 c) L = (Vt+Vi) * sqrt(abs(Vt-Vi)/Jm) 	... a short alternate form of b) assuming only positive values
 *
 *	 Notes: Ar = (Jm*T)/4					Ar is ramp acceleration
 *			T  = 2*sqrt((Vt-Vi)/Jm)			T is time
 *
 *			Assumes Vt, Vi and L are positive or zero
 *			Cannot assume Vt>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *			necessitating the introduction of fabs()
 *
 * 	_get_target_velocity() is a convenient function for determining Vt target 
 *	velocity for a given the initial velocity (Vi), length (L), and maximum jerk (Jm).
 *	Solving equation c) for Vt gives d)
 *
 *	 d) 1/3*((3*sqrt(3)*sqrt(27*Jm^2*L^4+32*Jm*L^2*Vi^3)+27*Jm*L^2+16*Vi^3)^(1/3)/2^(1/3) + 
 *      (4*2^(1/3)*Vi^2)/(3*sqrt(3)*sqrt(27*Jm^2*L^4+32*Jm*L^2*Vi^3)+27*Jm*L^2+16*Vi^3)^(1/3) - Vi)
 *
 *  FYI: Here's an expression that returns the jerk for a given deltaV (Vt-Vi) and L:
 * 	return(cube(deltaV / (pow(L, 0.66666666))));
 */
 /*
static float _get_target_length(const float Vi, const float Vt, const mpBuf_t *bf)
{
	return ((Vt+Vi) * sqrt(fabs(Vt-Vi) * bf->recip_jerk));
}

static float _get_target_velocity(const float Vi, const float L, const mpBuf_t *bf)
{
	float JmL2 = bf->jerk*square(L);
	float Vi2 = square(Vi);
	float Vi3x16 = 16*Vi*Vi2;
	float Ia = cbrt(3*sqrt(3) * sqrt(27*square(JmL2) + (2*JmL2*Vi3x16)) + 27*JmL2 + Vi3x16);
	return ((Ia/cbrt(2) + 4*cbrt(2)*Vi2/Ia - Vi)/3);
}
*/
/*
 * _get_junction_vmax() - Chamnit's algorithm - simple
 *
 *  Computes the maximum allowable junction speed by finding the velocity that will yield 
 *	the centripetal acceleration in the corner_acceleration value. The value of delta sets 
 *	the effective radius of curvature. Here's Chamnit's (Sungeun K. Jeon's) explanation 
 *	of what's going on:
 *
 *	"First let's assume that at a junction we only look a centripetal acceleration to simply 
 *	things. At a junction of two lines, let's place a circle such that both lines are tangent 
 *	to the circle. The circular segment joining the lines represents the path for constant 
 *	centripetal acceleration. This creates a deviation from the path (let's call this delta), 
 *	which is the distance from the junction to the edge of the circular segment. Delta needs 
 *	to be defined, so let's replace the term max_jerk with max_junction_deviation( or delta). 
 *	This indirectly sets the radius of the circle, and hence limits the velocity by the 
 *	centripetal acceleration. Think of the this as widening the race track. If a race car is 
 *	driving on a track only as wide as a car, it'll have to slow down a lot to turn corners. 
 *	If we widen the track a bit, the car can start to use the track to go into the turn. 
 *	The wider it is, the faster through the corner it can go.
 *
 *	If you do the geometry in terms of the known variables, you get:
 *		sin(theta/2) = R/(R+delta)  Re-arranging in terms of circle radius (R)
 *		R = delta*sin(theta/2)/(1-sin(theta/2). 
 *
 *	Theta is the angle between line segments given by: 
 *		cos(theta) = dot(a,b)/(norm(a)*norm(b)). 
 *
 *	Most of these calculations are already done in the planner. To remove the acos() 
 *	and sin() computations, use the trig half angle identity: 
 *		sin(theta/2) = +/- sqrt((1-cos(theta))/2). 
 *
 *	For our applications, this should always be positive. Now just plug the equations into 
 *	the centripetal acceleration equation: v_c = sqrt(a_max*R). You'll see that there are 
 *	only two sqrt computations and no sine/cosines."
 *
 *	How to compute the radius using brute-force trig:
 *		float theta = acos(costheta);
 *		float radius = delta * sin(theta/2)/(1-sin(theta/2));
 */
/*  This version function extends Chamnit's algorithm by computing a value for delta that 
 *	takes the contributions of the individual axes in the move into account. This allows 
 *	the control radius to vary by axis. This is necessary to support axes that have 
 *	different dynamics; such as a Z axis that doesn't move as fast as X and Y (such as a 
 *	screw driven Z axis on machine with a belt driven XY - like a Shapeoko), or rotary 
 *	axes ABC that have completely different dynamics than their linear counterparts.
 *
 *	The function takes the absolute values of the sum of the unit vector components as 
 *	a measure of contribution to the move, then scales the delta values from the non-zero 
 *	axes into a composite delta to be used for the move. Shown for an XY vector:
 *
 *	 	U[i]	Unit sum of i'th axis	fabs(unit_a[i]) + fabs(unit_b[i])
 *	 	Usum	Length of sums			Ux + Uy
 *	 	d		Delta of sums			(Dx*Ux+DY*UY)/Usum
 */
static float _get_junction_vmax(const float a_unit[], const float b_unit[])
{
	float costheta = - (a_unit[AXIS_X] * b_unit[AXIS_X]) - (a_unit[AXIS_Y] * b_unit[AXIS_Y]) 
					 - (a_unit[AXIS_Z] * b_unit[AXIS_Z]) - (a_unit[AXIS_A] * b_unit[AXIS_A]) 
					 - (a_unit[AXIS_B] * b_unit[AXIS_B]) - (a_unit[AXIS_C] * b_unit[AXIS_C]);

	if (costheta < -0.99) { return (10000000); } 		// straight line cases
	if (costheta > 0.99)  { return (0); } 				// reversal cases

	// Fuse the junction deviations into a vector sum
	float a_delta = square(a_unit[AXIS_X] * cm.a[AXIS_X].junction_dev);
	a_delta += square(a_unit[AXIS_Y] * cm.a[AXIS_Y].junction_dev);
	a_delta += square(a_unit[AXIS_Z] * cm.a[AXIS_Z].junction_dev);
	a_delta += square(a_unit[AXIS_A] * cm.a[AXIS_A].junction_dev);
	a_delta += square(a_unit[AXIS_B] * cm.a[AXIS_B].junction_dev);
	a_delta += square(a_unit[AXIS_C] * cm.a[AXIS_C].junction_dev);

	float b_delta = square(b_unit[AXIS_X] * cm.a[AXIS_X].junction_dev);
	b_delta += square(b_unit[AXIS_Y] * cm.a[AXIS_Y].junction_dev);
	b_delta += square(b_unit[AXIS_Z] * cm.a[AXIS_Z].junction_dev);
	b_delta += square(b_unit[AXIS_A] * cm.a[AXIS_A].junction_dev);
	b_delta += square(b_unit[AXIS_B] * cm.a[AXIS_B].junction_dev);
	b_delta += square(b_unit[AXIS_C] * cm.a[AXIS_C].junction_dev);

	float delta = (sqrt(a_delta) + sqrt(b_delta))/2;
	float sintheta_over2 = sqrt((1 - costheta)/2);
	float radius = delta * sintheta_over2 / (1-sintheta_over2);
	return(sqrt(radius * cm.junction_acceleration));
}

/*************************************************************************
 * feedholds - functions for performing holds
 *
 * mp_plan_hold_callback() - replan block list to execute hold
 * mp_end_hold() 		   - release the hold and restart block list
 *
 *	Feedhold is executed as cm.hold_state transitions executed inside 
 *	_exec_aline() and main loop callbacks to these functions:
 *	mp_plan_hold_callback() and mp_end_hold().
 */
/*	Holds work like this:
 * 
 * 	  - Hold is asserted by calling cm_feedhold() (usually invoked via a ! char)
 *		If hold_state is OFF and motion_state is RUNning it sets 
 *		hold_state to SYNC and motion_state to HOLD.
 *
 *	  - Hold state == SYNC tells the aline exec routine to execute the next aline 
 *		segment then set hold_state to PLAN. This gives the planner sufficient 
 *		time to replan the block list for the hold before the next aline segment 
 *		needs to be processed.
 *
 *	  - Hold state == PLAN tells the planner to replan the mr buffer, the current
 *		run buffer (bf), and any subsequent bf buffers as necessary to execute a
 *		hold. Hold planning replans the planner buffer queue down to zero and then
 *		back up from zero. Hold state is set to DECEL when planning is complete.
 *
 *	  - Hold state == DECEL persists until the aline execution runs to zero 
 *		velocity, at which point hold state transitions to HOLD.
 *
 *	  - Hold state == HOLD persists until the cycle is restarted. A cycle start 
 *		is an asynchronous event that sets the cycle_start_flag TRUE. It can 
 *		occur any time after the hold is requested - either before or after 
 *		motion stops.
 *
 *	  - mp_end_hold() is executed from cm_feedhold_sequencing_callback() once the 
 *		hold state == HOLD and a cycle_start has been requested.This sets the hold 
 *		state to OFF which enables _exec_aline() to continue processing. Move 
 *		execution begins with the first buffer after the hold.
 *
 *	Terms used:
 *	 - mr is the runtime buffer. It was initially loaded from the bf buffer
 *	 - bp+0 is the "companion" bf buffer to the mr buffer.
 *	 - bp+1 is the bf buffer following bp+0. This runs through bp+N
 *	 - bp (by itself) just refers to the current buffer being adjusted / replanned
 *
 *	Details: Planning re-uses bp+0 as an "extra" buffer. Normally bp+0 is returned 
 *		to the buffer pool as it is redundant once mr is loaded. Use the extra 
 *		buffer to split the move in two where the hold decelerates to zero. Use 
 *		one buffer to go to zero, the other to replan up from zero. All buffers past
 *		that point are unaffected other than that they need to be replanned for velocity.  
 *
 *	Note: There are multiple opportunities for more efficient organization of 
 *		  code in this module, but the code is so complicated I just left it
 *		  organized for clarity and hoped for the best from compiler optimization. 
 */

static float _compute_next_segment_velocity()
{
	if (mr.section == SECTION_BODY) { return (mr.segment_velocity);}
	return (mr.segment_velocity + mr.forward_diff_5);
}

stat_t mp_plan_hold_callback()
{
	if (cm.hold_state != FEEDHOLD_PLAN) { return (STAT_NOOP);}	// not planning a feedhold

	mpBuf_t *bp; 				// working buffer pointer
	if ((bp = mp_get_run_buffer()) == NULL) { return (STAT_NOOP);}	// Oops! nothing's running

	uint8_t mr_flag = true;		// used to tell replan to account for mr buffer Vx
	float mr_available_length;	// available length left in mr buffer for deceleration
	float braking_velocity;		// velocity left to shed to brake to zero
	float braking_length;		// distance required to brake to zero from braking_velocity

	// examine and process mr buffer
	mr_available_length = get_axis_vector_length(mr.target, mr.position);

/*	mr_available_length = 
		(sqrt(square(mr.endpoint[AXIS_X] - mr.position[AXIS_X]) +
			  square(mr.endpoint[AXIS_Y] - mr.position[AXIS_Y]) +
			  square(mr.endpoint[AXIS_Z] - mr.position[AXIS_Z]) +
			  square(mr.endpoint[AXIS_A] - mr.position[AXIS_A]) +
			  square(mr.endpoint[AXIS_B] - mr.position[AXIS_B]) +
			  square(mr.endpoint[AXIS_C] - mr.position[AXIS_C])));
*/

	// compute next_segment velocity
//	braking_velocity = mr.segment_velocity;
//	if (mr.section != SECTION_BODY) { braking_velocity += mr.forward_diff_1;}
	braking_velocity = _compute_next_segment_velocity();
	braking_length = _get_target_length(braking_velocity, 0, bp); // bp is OK to use here

	// Hack to prevent Case 2 moves for perfect-fit decels. Happens in homing situations
	// The real fix: The braking velocity cannot simply be the mr.segment_velocity as this
	// is the velocity of the last segment, not the one that's going to be executed next.
	// The braking_velocity needs to be the velocity of the next segment that has not yet 
	// been computed. In the mean time, this hack will work. 
	if ((braking_length > mr_available_length) && (fp_ZERO(bp->exit_velocity))) {
		braking_length = mr_available_length;
	}

	// Case 1: deceleration fits entirely into the length remaining in mr buffer
	if (braking_length <= mr_available_length) {
		// set mr to a tail to perform the deceleration
		mr.exit_velocity = 0;
		mr.tail_length = braking_length;
		mr.cruise_velocity = braking_velocity;
		mr.section = SECTION_TAIL;
		mr.section_state = SECTION_NEW;

		// re-use bp+0 to be the hold point and to run the remaining block length
		bp->length = mr_available_length - braking_length;
		bp->delta_vmax = _get_target_velocity(0, bp->length, bp);
		bp->entry_vmax = 0;						// set bp+0 as hold point
		bp->move_state = MOVE_NEW;				// tell _exec to re-use the bf buffer

		_reset_replannable_list();				// make it replan all the blocks
		_plan_block_list(mp_get_last_buffer(), &mr_flag);
		cm.hold_state = FEEDHOLD_DECEL;			// set state to decelerate and exit
		return (STAT_OK);
	}

	// Case 2: deceleration exceeds length remaining in mr buffer
	// First, replan mr to minimum (but non-zero) exit velocity

	mr.section = SECTION_TAIL;
	mr.section_state = SECTION_NEW;
	mr.tail_length = mr_available_length;
	mr.cruise_velocity = braking_velocity;
	mr.exit_velocity = braking_velocity - _get_target_velocity(0, mr_available_length, bp);	

	// Find the point where deceleration reaches zero. This could span multiple buffers.
	braking_velocity = mr.exit_velocity;		// adjust braking velocity downward
	bp->move_state = MOVE_NEW;					// tell _exec to re-use buffer
	for (uint8_t i=0; i<PLANNER_BUFFER_POOL_SIZE; i++) {// a safety to avoid wraparound
		mp_copy_buffer(bp, bp->nx);				// copy bp+1 into bp+0 (and onward...)
		if (bp->move_type != MOVE_TYPE_ALINE) {	// skip any non-move buffers
			bp = mp_get_next_buffer(bp);		// point to next buffer
			continue;
		}
		bp->entry_vmax = braking_velocity;		// velocity we need to shed
		braking_length = _get_target_length(braking_velocity, 0, bp);

		if (braking_length > bp->length) {		// decel does not fit in bp buffer
			bp->exit_vmax = braking_velocity - _get_target_velocity(0, bp->length, bp);
			braking_velocity = bp->exit_vmax;	// braking velocity for next buffer
			bp = mp_get_next_buffer(bp);		// point to next buffer
			continue;
		}
		break;
	}
	// Deceleration now fits in the current bp buffer
	// Plan the first buffer of the pair as the decel, the second as the accel
	bp->length = braking_length;
	bp->exit_vmax = 0;

	bp = mp_get_next_buffer(bp);				// point to the acceleration buffer
	bp->entry_vmax = 0;
	bp->length -= braking_length;				// the buffers were identical (and hence their lengths)
	bp->delta_vmax = _get_target_velocity(0, bp->length, bp);
	bp->exit_vmax = bp->delta_vmax;

	_reset_replannable_list();					// make it replan all the blocks
	_plan_block_list(mp_get_last_buffer(), &mr_flag);
	cm.hold_state = FEEDHOLD_DECEL;				// set state to decelerate and exit
	return (STAT_OK);
}

/*
 * mp_end_hold() - end a feedhold
 */
stat_t mp_end_hold()
{
	if (cm.hold_state == FEEDHOLD_END_HOLD) { 
		cm.hold_state = FEEDHOLD_OFF;
		mpBuf_t *bf;
		if ((bf = mp_get_run_buffer()) == NULL) {	// NULL means nothing's running
			cm_set_motion_state(MOTION_STOP);
			return (STAT_NOOP);
		}
		cm.motion_state = MOTION_RUN;
		st_request_exec_move();					// restart the steppers
	}
	return (STAT_OK);
}


/********************************/
/********************************/
/****** PLANNER UNIT TESTS ******/
/********************************/
/********************************/

#ifdef __UNIT_TESTS
#ifdef __UNIT_TEST_PLANNER

// Comment in and out to enable/disable parts of the unit tests
//#define __TEST_GET_TARGET_LENGTH
//#define __TEST_GET_TARGET_VELOCITY
#define __TEST_CALCULATE_TRAPEZOID
//#define __TEST_GET_JUNCTION_VMAX

// Prototypes and local defines
#ifdef __TEST_GET_TARGET_LENGTH
static void _test_get_target_length(void);
#endif

#ifdef __TEST_GET_TARGET_VELOCITY
static void _test_get_target_velocity(void);
#endif

#ifdef __TEST_CALCULATE_TRAPEZOID
static void _test_trapezoid(float length, float Ve, float Vt, float Vx, mpBuf_t *bf);
static void _test_calculate_trapezoid(void);
#endif

#ifdef __TEST_GET_JUNCTION_VMAX
static void _make_unit_vector(float unit[], float x, float y, float z, float a, float b, float c);
static void _test_get_junction_vmax(void);
#endif

//#define JERK_TEST_VALUE (float)20000000	// set this to the value in the profile you are running
//#define JERK_TEST_VALUE (float)50000000	// set this to the value in the profile you are running
//#define JERK_TEST_VALUE (float)100000000	// set this to the value in the profile you are running
//static void _set_jerk(const float jerk, mpBuf_t *bf);

void mp_unit_tests()
{
#ifdef __TEST_GET_TARGET_LENGTH
	_test_get_target_length();
#endif

#ifdef __TEST_GET_TARGET_VELOCITY
	_test_get_target_velocity();
#endif

#ifdef __TEST_CALCULATE_TRAPEZOID
	_test_calculate_trapezoid();
#endif

#ifdef __TEST_GET_JUNCTION_VMAX
	_test_get_junction_vmax();
#endif
}

/************************************/
/***** __TEST_GET_TARGET_LENGTH *****/
/************************************/

#ifdef __TEST_GET_TARGET_LENGTH
static void _test_get_target_length()
{
	mpBuf_t *bf = mp_get_write_buffer();
	bf->jerk = 1800000;
	bf->recip_jerk = 1/bf->jerk;
	float L;
	float Vi;
	float Vt;

	Vi = 0;
	Vt = 300;
	L = _get_target_length(Vi, Vt, bf);		// result: L = 3.872983
	Vt = _get_target_velocity(Vi, L, bf);	// result: Vt = 300

	Vi = 165;
	Vt = 300;
	L = _get_target_length(Vi, Vt, bf);		// result: L = 4.027018
	Vt = _get_target_velocity(Vi, L, bf);	// result: Vt = 300

	Vi = 523;
	Vt = 600;
	L = _get_target_length(Vi, Vt, bf);		// result: L = 7.344950
	Vt = _get_target_velocity(Vi, L, bf);	// result: Vt = 600

	Vi = 200;
	Vt = 400;
	L = _get_target_length(Vi, Vt, bf);		// result: L = 6.324555
	Vt = _get_target_velocity(Vi, L, bf);	// result: Vt = 400

	Vi = 174;
	Vt = 347;
	L = _get_target_length(Vi, Vt, bf);		// result: L = 5.107690
	Vt = _get_target_velocity(Vi, L, bf);	// result: Vt = 347
}
#endif	// __TEST_GET_TARGET_LENGTH


/**************************************/
/***** __TEST_GET_TARGET_VELOCITY *****/
/**************************************/

#ifdef __TEST_GET_TARGET_VELOCITY
static void _test_get_target_velocity()
{
	mpBuf_t *bf = mp_get_write_buffer();

	float L = 3.872983;
	float Vi = 0;
	float Vt; 			// 300
	bf->jerk = 1800000;

	Vt = _get_target_velocity(Vi, L, bf);
}
#endif	// __TEST_GET_TARGET_VELOCITY


/**************************************/
/***** __TEST_CALCULATE_TRAPEZOID *****/
/**************************************/

/* These tests are calibrated for settings_default.h profile values:

#define JERK_MAX 				20			// "20,000,000" mm/(min^3), see #define below
#define JUNCTION_DEVIATION		0.05		// default value, in mm
#define JUNCTION_ACCELERATION	100000		// centripetal acceleration around corners

Other assumptions include:

#define NOM_SEGMENT_USEC 	((float)5000)		// nominal segment time
#define MIN_SEGMENT_USEC 	((float)2500)		// minimum segment time / minimum move time

*/

#ifdef __TEST_CALCULATE_TRAPEZOID

#define JERK_TEST_VALUE (float)20000000		// set this to the value in the profile you are running

static void _test_trapezoid(float length, float Ve, float Vt, float Vx, mpBuf_t *bf)
{
	bf->length = length;
	bf->entry_velocity = Ve;
	bf->cruise_velocity = Vt;
	bf->exit_velocity = Vx;
	bf->cruise_vmax = Vt;
	bf->jerk = JERK_TEST_VALUE;
	bf->recip_jerk = 1/bf->jerk;
	bf->cbrt_jerk = cbrt(bf->jerk);
	_calculate_trapezoid(bf);
}

static void _test_calculate_trapezoid()
{
	mpBuf_t *bf = mp_get_write_buffer();

//	_test_trapezoid(0.0001,	800,	800, 	800,bf);	// B case
//	_test_trapezoid(0.001,	800,	800, 	800,bf);	// B case
//	_test_trapezoid(0.01,	800,	800, 	800,bf);	// B case
	_test_trapezoid(0.05,	800,	800, 	800,bf);	// B case
	_test_trapezoid(0.1,	800,	800, 	800,bf);	// B case
	_test_trapezoid(1.0,	800,	800, 	800,bf);	// B case
	_test_trapezoid(10.0,	800,	800, 	800,bf);	// B case

// F cases: line below minimum velocity.
//				   	L	 	Ve  	Vt		Vx
//	_test_trapezoid(0.0001, 0,		100,	0,		bf);
//	_test_trapezoid(0.001, 	0,		100,	0,		bf);
	_test_trapezoid(0.0001, 1000,	1000,	1000,	bf);	// min length = 0.0833...
	_test_trapezoid(0.001, 	1000,	1000,	1000,	bf);	// min length = 0.0833..
	_test_trapezoid(0.01, 	1000,	1000,	1000,	bf);	// min length = 0.0833..


// B cases: body-only line above minimum velocity. At std settings nominal length == 0.00833333
//				   	L	 	Ve  	Vt		Vx
	_test_trapezoid(0.08, 	1000,	1000,	1000,	bf);	// min length = 0.0833..
	_test_trapezoid(0.09, 	1000,	1000,	1000,	bf);	// min length = 0.0833..
	_test_trapezoid(0.009, 	0,		100,	0,		bf);

	_test_trapezoid(0.1, 	0,	100,	0,	bf);

// no-fit cases: line below minimum velocity or length
//				   	L	 Ve  	Vt		Vx
	_test_trapezoid(1.0, 0,		0.001,	0,	bf);
	_test_trapezoid(0.0, 0,		100,	0,	bf);
	_test_trapezoid(0.01, 0,	100,	0,	bf);

// 1 section cases (H,B and T)
//				   	L	 Ve  	Vt		Vx
	_test_trapezoid(1.0, 800,	800, 	800,bf);	// B case
	_test_trapezoid(0.8, 0,		400, 	0, bf);		// B case
	_test_trapezoid(0.8, 200,	400, 	0, bf);
	_test_trapezoid(2.0, 400,	400, 	0, bf);
	_test_trapezoid(0.8, 0,		400, 	200,bf);

// 2 section cases (HT)
//				   	L   Ve  	Vt		Vx
	_test_trapezoid(0.8, 0,		200, 	0, bf);		// requested fit HT case (exact fit)
	_test_trapezoid(0.8, 0,		400, 	0, bf);		// symmetric rate-limited HT case
	_test_trapezoid(0.8, 200,	400, 	0, bf);		// asymmetric rate-limited HT case
	_test_trapezoid(2.0, 400,	400, 	0, bf);
	_test_trapezoid(0.8, 0,		400, 	200,bf);

// requested-fit cases
//				   	L  	 Ve  	Vt		Vx
	_test_trapezoid(0.8, 400,	400, 	0, 	 bf);
	_test_trapezoid(0.8, 600,	600, 	200, bf);
	_test_trapezoid(0.8, 0,		400, 	400, bf);
	_test_trapezoid(0.8, 200,	600, 	600, bf);

// HBT - 3 section cases
//				   	L    Ve  	Vt		Vx
	_test_trapezoid(0.8, 0,		190, 	0, bf);
	_test_trapezoid(2.0, 200,	400, 	0, bf);



/*
// test cases drawn from Mudflap
//				   	L		Ve  	  Vt		Vx
	_test_trapezoid(0.6604, 000.000,  800.000,  000.000, bf);	// line 50
	_test_trapezoid(0.8443, 000.000,  805.855,  000.000, bf);	// line 55
	_test_trapezoid(0.8443, 000.000,  805.855,  393.806, bf);	// line 55'
	_test_trapezoid(0.7890, 393.805,  955.829,  000.000, bf);	// line 60
	_test_trapezoid(0.7890, 393.806,  955.829,  390.294, bf);	// line 60'
	_test_trapezoid(0.9002, 390.294,  833.884,  000.000, bf);	// line 65

	_test_trapezoid(0.9002, 390.294,  833.884,  455.925, bf);	// line 65'
	_test_trapezoid(0.9002, 390.294,  833.884,  806.895, bf);	// line 65"
	_test_trapezoid(0.9735, 455.925,  806.895,  000.000, bf);	// line 70
	_test_trapezoid(0.9735, 455.925,  806.895,  462.101, bf);	// line 70'

	_test_trapezoid(0.9735, 806.895,  806.895,  802.363, bf);	// line 70"

	_test_trapezoid(0.9935, 462.101,  802.363,  000.000, bf);	// line 75
	_test_trapezoid(0.9935, 462.101,  802.363,  000.000, bf);	// line 75'
	_test_trapezoid(0.9935, 802.363,  802.363,  477.729, bf);	// line 75"
	_test_trapezoid(0.9935, 802.363,  802.363,  802.363, bf);	// line 75"
	_test_trapezoid(1.0441, 477.729,  843.274,  000.000, bf);	// line 80
	_test_trapezoid(1.0441, 802.363,  843.274,  388.515, bf);	// line 80'
	_test_trapezoid(1.0441, 802.363,  843.274,  803.990, bf);	// line 80"
	_test_trapezoid(0.7658, 388.515,  803.990,  000.000, bf);	// line 85
	_test_trapezoid(0.7658, 803.990,  803.990,  733.618, bf);	// line 85'
	_test_trapezoid(0.7658, 803.990,  803.990,  802.363, bf);	// line 85"
	_test_trapezoid(1.9870, 733.618,  802.363,  000.000, bf);	// line 90
	_test_trapezoid(1.9870, 802.363,  802.363,  727.371, bf);	// line 90'
	_test_trapezoid(1.9870, 802.363,  802.363,  802.363, bf);	// line 90'
	_test_trapezoid(1.9617, 727.371,  802.425,  000.000, bf);	// line 95
	_test_trapezoid(1.9617, 727.371,  802.425,  000.000, bf);	// line 95'
	_test_trapezoid(1.9617, 802.363,  802.425,  641.920, bf);	// line 95"
	_test_trapezoid(1.9617, 802.363,  802.425,  802.425, bf);	// line 95"'
	_test_trapezoid(1.6264, 641.920,  826.209,  000.000, bf);	// line 100
	_test_trapezoid(1.6264, 802.425,  826.209,  266.384, bf);	// line 100'
	_test_trapezoid(1.6264, 802.425,  826.209,  658.149, bf);	// line 100"
	_test_trapezoid(1.6264, 802.425,  826.209,  679.360, bf);	// line 100"'
	_test_trapezoid(0.4348, 266.384,  805.517,  000.000, bf);	// line 105
	_test_trapezoid(0.4348, 658.149,  805.517,  391.765, bf);	// line 105'
	_test_trapezoid(0.4348, 679.360,  805.517,  412.976, bf);	// line 105"
	_test_trapezoid(0.7754, 391.765,  939.343,  000.000, bf);	// line 110
	_test_trapezoid(0.7754, 412.976,  939.343,  376.765, bf);	// line 110'
	_test_trapezoid(0.7754, 802.425,  826.209,  679.360, bf);	// line 110"
	_test_trapezoid(0.7754, 412.976,  939.343,  804.740, bf);	// line 110"'
	_test_trapezoid(0.7313, 376.765,  853.107,  000.000, bf);	// line 115
	_test_trapezoid(0.7313, 804.740,  853.107,  437.724, bf);	// line 115'
	_test_trapezoid(0.7313, 804.740,  853.107,  683.099, bf);	// line 115"
	_test_trapezoid(0.7313, 804.740,  853.107,  801.234, bf);	// line 115"'
	_test_trapezoid(0.9158, 437.724,  801.233,  000.000, bf);	// line 120
	_test_trapezoid(0.9158, 683.099,  801.233,  245.375, bf);	// line 120'
	_test_trapezoid(0.9158, 801.233,  801.233,  617.229, bf);	// line 120"
	_test_trapezoid(0.3843, 245.375,  807.080,  000.000, bf);	// line 125
	_test_trapezoid(0.3843, 617.229,  807.080,  371.854, bf);	// line 125'  6,382,804 cycles
*/

/*
// test cases drawn from braid_600mm					 		// expected results
//				   	L   	Ve  		Vt		Vx
	_test_trapezoid(0.327,	000.000,	600,	000.000, bf); // Ve=0 	   	Vc=110.155
	_test_trapezoid(0.327,	000.000,	600,	174.538, bf); // Ve=0, 	   	Vc=174.744	Vx=174.537
	_test_trapezoid(0.327,	174.873,	600,	173.867, bf); // Ve=174.873	Vc=185.356	Vx=173.867
	_test_trapezoid(0.327,	173.593,	600,	000.000, bf); // Ve=174.873	Vc=185.356	Vx=173.867
	_test_trapezoid(0.327,	347.082,	600,	173.214, bf); // Ve=174.873	Vc=185.356	Vx=173.867
*/
}

#endif // __TEST_CALCULATE_TRAPEZOID


/************************************/
/***** __TEST_GET_JUNCTION_VMAX *****/
/************************************/

#ifdef __TEST_GET_JUNCTION_VMAX

static void _make_unit_vector(float unit[], float x, float y, float z, float a, float b, float c)
{
	float length = sqrt(x*x + y*y + z*z + a*a + b*b + c*c);
	unit[AXIS_X] = x/length;
	unit[AXIS_Y] = y/length;
	unit[AXIS_Z] = z/length;
	unit[AXIS_A] = a/length;
	unit[AXIS_B] = b/length;
	unit[AXIS_C] = c/length;
}

static void _test_get_junction_vmax()
{
//	cfg.a[AXIS_X].jerk_max = JERK_TEST_VALUE;
//	cfg.a[AXIS_Y].jerk_max = JERK_TEST_VALUE;
//	cfg.a[AXIS_Z].jerk_max = JERK_TEST_VALUE;
//	cfg.a[AXIS_A].jerk_max = JERK_TEST_VALUE;
//	cfg.a[AXIS_B].jerk_max = JERK_TEST_VALUE;
//	cfg.a[AXIS_C].jerk_max = JERK_TEST_VALUE;
//	mm.jerk_transition_size = 0.5;
//	mm.jerk_limit_max = 184.2;
/*
	mm.test_case = 1;				// straight line along X axis
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 2;				// angled straight line
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 3;				// 5 degree bend
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.9962, 0.0872, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 4;				// 30 degrees
	_make_unit_vector(mm.a_unit, 1.0000, 0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.8660, 0.5000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 5;				// 45 degrees
	_make_unit_vector(mm.a_unit, 0.8660,	0.5000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.2588,	0.9659, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 6;				// 60 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.5000,	0.8660, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 7;				// 90 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit, 0.0000,	1.0000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 8;				// 90 degrees rotated 45 degrees
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.7071, 0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 9;				// 120 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.5000,	0.8660, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 10;				// 150 degrees
	_make_unit_vector(mm.a_unit, 1.0000,	0.0000, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.8660,	0.5000, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);

	mm.test_case = 11;				// 180 degrees
	_make_unit_vector(mm.a_unit, 0.7071, 0.7071, 0, 0, 0, 0);
	_make_unit_vector(mm.b_unit,-0.7071,-0.7071, 0, 0, 0, 0);
	mm.test_velocity = _get_junction_vmax(mm.a_unit, mm.b_unit);
*/
}
#endif // __TEST_GET_JUNCTION_VMAX
#endif // __UNIT_TEST_PLANNER
#endif // __UNIT_TESTS

#ifdef __cplusplus
}
#endif

