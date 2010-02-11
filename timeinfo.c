#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#define DEBUG

#include "debug.h"
#include "timeinfo.h"

#define MAX_NET_LAG 2.0 /* Max net lag in seconds. TODO: estimate dynamically. */
#define RESERVED_BYOYOMI_PERCENT 15 /* Reserve 15% of byoyomi time as safety margin if risk of losing on time */

/* For safety, use at most 3 times the desired time on a single move
 * in main time, and 1.1 times in byoyomi. */
#define MAX_MAIN_TIME_EXTENSION 3.0
#define MAX_BYOYOMI_TIME_EXTENSION 1.1

bool
time_parse(struct time_info *ti, char *s)
{
	switch (s[0]) {
		case '_': ti->period = TT_TOTAL; s++; break;
		default: ti->period = TT_MOVE; break;
	}
	switch (s[0]) {
		case '=':
			ti->dim = TD_GAMES;
			ti->len.games = atoi(++s);
			break;
		default:
			if (!isdigit(s[0]))
				return false;
			ti->dim = TD_WALLTIME;
			ti->len.t.timer_start = 0;
			if (ti->period == TT_TOTAL) {
				ti->len.t.main_time = atof(s);
				ti->len.t.byoyomi_time = 0.0;
				ti->len.t.byoyomi_time_max = 0.0;
				ti->len.t.byoyomi_periods = 0;
				ti->len.t.byoyomi_stones = 0;
				ti->len.t.byoyomi_stones_max = 0;
			} else { assert(ti->period == TT_MOVE);
				ti->len.t.main_time = 0.0;
				ti->len.t.byoyomi_time = atof(s);
				ti->len.t.byoyomi_time_max = ti->len.t.byoyomi_time;
				ti->len.t.byoyomi_periods = 1;
				ti->len.t.byoyomi_stones = 1;
				ti->len.t.byoyomi_stones_max = 1;
			}
			break;
	}
	return true;
}

/* Update time settings according to gtp time_settings or kgs-time_settings command. */
void
time_settings(struct time_info *ti, int main_time, int byoyomi_time, int byoyomi_stones, int byoyomi_periods)
{
	if (main_time < 0) {
		ti->period = TT_NULL; // no time limit, rely on engine default
	} else {
		ti->period = main_time > 0 ? TT_TOTAL : TT_MOVE;
		ti->dim = TD_WALLTIME;
		ti->len.t.timer_start = 0;
		ti->len.t.main_time = (double) main_time;
		ti->len.t.byoyomi_time = (double) byoyomi_time;
		ti->len.t.byoyomi_periods = byoyomi_periods;
		ti->len.t.byoyomi_stones = byoyomi_stones;
		ti->len.t.canadian = byoyomi_stones > 0;
		if (byoyomi_time > 0) {
			/* Normally, only one of byoyomi_periods and
			 * byoyomi_stones arguments will be > 0. However,
			 * our data structure uses generalized byoyomi
			 * specification that will assume "1 byoyomi period
			 * of N stones" for Canadian byoyomi and "N byoyomi
			 * periods of 1 stone" for Japanese byoyomi. */
			if (ti->len.t.byoyomi_periods < 1)
				ti->len.t.byoyomi_periods = 1;
			if (ti->len.t.byoyomi_stones < 1)
				ti->len.t.byoyomi_stones = 1;
		} else {
			assert(!ti->len.t.byoyomi_periods && !ti->len.t.byoyomi_stones);
		}
		ti->len.t.byoyomi_time_max = ti->len.t.byoyomi_time;
		ti->len.t.byoyomi_stones_max = ti->len.t.byoyomi_stones;
	}
}

/* Update time information according to gtp time_left command.
 * kgs doesn't give time_left for the first move, so make sure
 * that just time_settings + time_stop_conditions still work. */
void
time_left(struct time_info *ti, int time_left, int stones_left)
{
	assert(ti->period != TT_NULL);
	ti->dim = TD_WALLTIME;

	if (!time_left && !stones_left) {
		/* Some GTP peers send time_left 0 0 at the end of main time. */
		ti->period = TT_MOVE;
		ti->len.t.main_time = 0;
		/* byoyomi_time kept fully charged. */

	} else if (!stones_left) {
		/* Main time */
		ti->period = TT_TOTAL;
		ti->len.t.main_time = time_left;
		/* byoyomi_time kept fully charged. */

	} else {
		/* Byoyomi */
		ti->period = TT_MOVE;
		ti->len.t.main_time = 0;
		ti->len.t.byoyomi_time = time_left;
		if (ti->len.t.canadian) {
			ti->len.t.byoyomi_stones = stones_left;
		} else {
			// field misused by kgs
			ti->len.t.byoyomi_periods = stones_left;
		}
	}
}

/* Start our timer. kgs does this (correctly) on "play" not "genmove"
 * unless we are making the first move of the game. */
void
time_start_timer(struct time_info *ti)
{
	if (ti->period != TT_NULL && ti->dim == TD_WALLTIME)
		ti->len.t.timer_start = time_now();
}

void
time_sub(struct time_info *ti, double interval)
{
	assert(ti->dim == TD_WALLTIME && ti->period != TT_NULL);

	if (ti->period == TT_TOTAL) {
		ti->len.t.main_time -= interval;
		if (ti->len.t.main_time >= 0)
			return;
		/* Fall-through to byoyomi. */
		ti->period = TT_MOVE;
		interval = -ti->len.t.main_time;
		ti->len.t.main_time = 0;
	}

	ti->len.t.byoyomi_time -= interval;
	if (ti->len.t.byoyomi_time < 0) {
		/* Lost a period. */
		if (--ti->len.t.byoyomi_periods < 1) {
			fprintf(stderr, "*** LOST ON TIME internally! (%0.2f, spent %0.2fs on last move)\n",
				ti->len.t.byoyomi_time, interval);
			/* Well, what can we do? Pretend this didn't happen. */
			ti->len.t.byoyomi_periods = 1;
		}
		ti->len.t.byoyomi_time = ti->len.t.byoyomi_time_max;
		ti->len.t.byoyomi_stones = ti->len.t.byoyomi_stones_max;
		return;
	}
	if (--ti->len.t.byoyomi_stones < 1) {
		/* Finished a period. */
		ti->len.t.byoyomi_time = ti->len.t.byoyomi_time_max;
		ti->len.t.byoyomi_stones = ti->len.t.byoyomi_stones_max;
	}
}

/* Returns the current time. */
double
time_now(void)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	return now.tv_sec + now.tv_nsec/1000000000.0;
}

/* Sleep for a given interval (in seconds). Return immediately if interval < 0. */
void
time_sleep(double interval)
{
	struct timespec ts;
	double sec;
	ts.tv_nsec = (int)(modf(interval, &sec)*1000000000.0);
        ts.tv_sec = (int)sec;
	nanosleep(&ts, NULL); /* ignore error if interval was < 0 */
}


/* Returns true if we are in byoyomi (or should play as if in byo yomi
 * because remaining time per move in main time is less than byoyomi time
 * per move). */
static bool
time_in_byoyomi(struct time_info *ti) {
	assert(ti->dim == TD_WALLTIME);
	if (!ti->len.t.byoyomi_time)
		return false; // there is no byoyomi!
	assert(ti->len.t.byoyomi_stones > 0);
	if (!ti->len.t.main_time)
		return true; // we _are_ in byoyomi
	if (ti->len.t.main_time <= ti->len.t.byoyomi_time / ti->len.t.byoyomi_stones + 0.001)
		return true; // our basic time left is less than byoyomi time per move
	return false;
}

/* Set worst.time to all available remaining time (main time plus usable
 * byoyomi), to be spread over returned number of moves (expected game
 * length minus moves to be played in final byoyomi - if we would not be
 * able to spend more time on them in main time anyway). */
static int
time_stop_set_remaining(struct time_info *ti, struct board *b, double net_lag, struct time_stop *stop)
{
	int moves_left = board_estimated_moves_left(b);
	stop->worst.time = ti->len.t.main_time;

	/* If we have byoyomi available, plan to extend our thinking
	 * time to make use of it. */
	if (ti->len.t.byoyomi_time > 0) {
		assert(ti->len.t.byoyomi_stones > 0);
		/* Time for one move in byoyomi. */
		double move_time = ti->len.t.byoyomi_time / ti->len.t.byoyomi_stones;

		/* For Japanese byoyomi with N>1 periods, we use N-1
		 * periods as main time, keeping the last one as
		 * insurance against unexpected net lag. */
		if (ti->len.t.byoyomi_periods > 2) {
			stop->worst.time += (ti->len.t.byoyomi_periods - 2) * move_time;
			// Will add 1 more byoyomi_time just below
		}

		/* In case of Canadian byoyomi, include time that can
		 * be spent on its first move. */
		stop->worst.time += move_time;

		/* Maximize the number of moves played uniformly in
		 * main time, while not playing faster in main time
		 * than in byoyomi. At this point, the main time
		 * remaining is stop->worst.time and already includes
		 * the first (canadian) or N-1 byoyomi periods. */
		double actual_byoyomi = move_time - net_lag;
		if (actual_byoyomi > 0) {
			int main_moves = stop->worst.time / actual_byoyomi;
			if (moves_left > main_moves) {
				/* We plan to do too many moves in
				 * main time, do the rest in byoyomi. */
				moves_left = main_moves;
			}
			if (moves_left <= 0) // possible if too much lag
				moves_left = 1;
		}
	}

	return moves_left;
}

/* Adjust the recommended per-move time based on the current game phase.
 * We expect stop->worst to be total time available, stop->desired the current
 * per-move time allocation, and set stop->desired to adjusted per-move time. */
static void
time_stop_phase_adjust(struct board *b, int fuseki_end, int yose_start, struct time_stop *stop)
{
	int bsize = (board_size(b)-2)*(board_size(b)-2);
	fuseki_end = fuseki_end * bsize / 100; // move nb at fuseki end
	yose_start = yose_start * bsize / 100; // move nb at yose start
	assert(fuseki_end < yose_start);

	/* No adjustments in yose. */
	if (b->moves >= yose_start)
		return;
	int moves_to_yose = (yose_start - b->moves) / 2;
	// ^- /2 because we only consider the moves we have to play ourselves
	int left_at_yose_start = board_estimated_moves_left(b) - moves_to_yose;
	if (left_at_yose_start < MIN_MOVES_LEFT)
		left_at_yose_start = MIN_MOVES_LEFT;

	/* This particular value of middlegame_time will continuously converge
	 * to effective "yose_time" value as we approach yose_start. */
	double middlegame_time = stop->worst.time / left_at_yose_start;
	if (middlegame_time < stop->desired.time)
		return;

	if (b->moves < fuseki_end) {
		assert(fuseki_end > 0);
		/* At the game start, use stop->desired.time (rather
		 * conservative estimate), then gradually prolong it. */
		double beta = b->moves / fuseki_end;
		stop->desired.time = middlegame_time * beta + stop->desired.time * (1 - beta);

	} else { assert(b->moves < yose_start);
		/* Middlegame, start with relatively large value, then
		 * converge to the uniform-timeslice yose value. */
		stop->desired.time = middlegame_time;
	}
}

/* Pre-process time_info for search control and sets the desired stopping conditions. */
void
time_stop_conditions(struct time_info *ti, struct board *b, int fuseki_end, int yose_start, struct time_stop *stop)
{
	/* We must have _some_ limits by now, be it random default values! */
	assert(ti->period != TT_NULL);

	/* Special-case limit by number of simulations. */
	if (ti->dim == TD_GAMES) {
		if (ti->period == TT_TOTAL) {
			ti->period = TT_MOVE;
			ti->len.games /= board_estimated_moves_left(b);
		}

		stop->desired.playouts = ti->len.games;
		/* We force worst == desired, so note that we will NOT loop
		 * until best == winner. */
		stop->worst.playouts = ti->len.games;
		return;
	}

	assert(ti->dim == TD_WALLTIME);


	/* Minimum net lag (seconds) to be reserved in the time for move. */
	double net_lag = MAX_NET_LAG;
	if (!ti->len.t.timer_start) {
		ti->len.t.timer_start = time_now(); // we're playing the first game move
	} else {
		net_lag += time_now() - ti->len.t.timer_start;
		// TODO: keep statistics to get good estimate of lag not just current move
	}


	if (ti->period == TT_TOTAL && time_in_byoyomi(ti)) {
		/* Technically, we are still in main time, but we can
		 * effectively switch to byoyomi scheduling since we
		 * have less time available than one byoyomi move takes. */
		ti->period = TT_MOVE;
	}


	if (ti->period == TT_MOVE) {
		/* We are in byoyomi, or almost! */

		/* The period can still include some tiny remnant of main
		 * time if we are just switching to byoyomi. */
		double period_len = ti->len.t.byoyomi_time + ti->len.t.main_time;

		stop->worst.time = period_len;
		assert(ti->len.t.byoyomi_stones > 0);
		stop->desired.time = period_len / ti->len.t.byoyomi_stones;

		/* Use a larger safety margin if we risk losing on time on
		 * this move; it makes no sense to have 30s byoyomi and wait
		 * until 28s to play our move). */
		if (stop->desired.time >= period_len - net_lag) {
			double safe_margin = RESERVED_BYOYOMI_PERCENT * stop->desired.time / 100;
			if (safe_margin > net_lag)
				net_lag = safe_margin;
		}

		/* Make recommended_old == average(recommended_new, max) */
		double worst_time = stop->desired.time * MAX_BYOYOMI_TIME_EXTENSION;
		if (worst_time < stop->worst.time)
			stop->worst.time = worst_time;
		stop->desired.time *= (2 - MAX_BYOYOMI_TIME_EXTENSION);

	} else { assert(ti->period == TT_TOTAL);
		/* We are in main time. */

		assert(ti->len.t.main_time > 0);
		/* Set worst.time to all available remaining time, to be spread
		 * over returned number of moves. */
		int moves_left = time_stop_set_remaining(ti, b, net_lag, stop);

		/* Allocate even slice of the remaining time for next move. */
		stop->desired.time = stop->worst.time / moves_left;
		assert(stop->desired.time > 0 && stop->worst.time > 0);
		assert(stop->desired.time <= stop->worst.time + 0.001);

		/* Furthermore, tweak the slice based on the game phase. */
		time_stop_phase_adjust(b, fuseki_end, yose_start, stop);

		/* Put final upper bound on maximal time spent on the move. */
		double worst_time = stop->desired.time * MAX_MAIN_TIME_EXTENSION;
		if (worst_time < stop->worst.time)
			stop->worst.time = worst_time;
		if (stop->desired.time > stop->worst.time)
			stop->desired.time = stop->worst.time;
	}

	if (DEBUGL(1))
		fprintf(stderr, "desired %0.2f, worst %0.2f, clock [%d] %0.2f + %0.2f/%d*%d, lag %0.2f\n",
			stop->desired.time, stop->worst.time,
			ti->dim, ti->len.t.main_time,
			ti->len.t.byoyomi_time, ti->len.t.byoyomi_stones,
			ti->len.t.byoyomi_periods, net_lag);

	/* Account for lag. */
	stop->desired.time -= net_lag;
	stop->worst.time -= net_lag;
}