/* dive.c */
/* maintains the internal dive list structure */
#include <string.h>
#include <stdio.h>
#include <glib/gi18n.h>

#include "dive.h"

void add_event(struct divecomputer *dc, int time, int type, int flags, int value, const char *name)
{
	struct event *ev, **p;
	unsigned int size, len = strlen(name);

	size = sizeof(*ev) + len + 1;
	ev = malloc(size);
	if (!ev)
		return;
	memset(ev, 0, size);
	memcpy(ev->name, name, len);
	ev->time.seconds = time;
	ev->type = type;
	ev->flags = flags;
	ev->value = value;
	ev->next = NULL;

	p = &dc->events;
	while (*p)
		p = &(*p)->next;
	*p = ev;
	remember_event(name);
}

int get_pressure_units(unsigned int mb, const char **units)
{
	int pressure;
	const char* unit;

	switch (output_units.pressure) {
	case PASCAL:
		pressure = mb * 100;
		unit = _("pascal");
		break;
	case BAR:
		pressure = (mb + 500) / 1000;
		unit = _("bar");
		break;
	case PSI:
		pressure = mbar_to_PSI(mb);
		unit = _("psi");
		break;
	}
	if (units)
		*units = unit;
	return pressure;
}

double get_temp_units(unsigned int mk, const char **units)
{
	double deg;
	const char *unit;

	if (output_units.temperature == FAHRENHEIT) {
		deg = mkelvin_to_F(mk);
		unit = UTF8_DEGREE "F";
	} else {
		deg = mkelvin_to_C(mk);
		unit = UTF8_DEGREE "C";
	}
	if (units)
		*units = unit;
	return deg;
}

double get_volume_units(unsigned int ml, int *frac, const char **units)
{
	int decimals;
	double vol;
	const char *unit;

	switch (output_units.volume) {
	case LITER:
		vol = ml / 1000.0;
		unit = _("l");
		decimals = 1;
		break;
	case CUFT:
		vol = ml_to_cuft(ml);
		unit = _("cuft");
		decimals = 2;
		break;
	}
	if (frac)
		*frac = decimals;
	if (units)
		*units = unit;
	return vol;
}

double get_depth_units(unsigned int mm, int *frac, const char **units)
{
	int decimals;
	double d;
	const char *unit;

	switch (output_units.length) {
	case METERS:
		d = mm / 1000.0;
		unit = _("m");
		decimals = d < 20;
		break;
	case FEET:
		d = mm_to_feet(mm);
		unit = _("ft");
		decimals = 0;
		break;
	}
	if (frac)
		*frac = decimals;
	if (units)
		*units = unit;
	return d;
}

double get_weight_units(unsigned int grams, int *frac, const char **units)
{
	int decimals;
	double value;
	const char* unit;

	if (output_units.weight == LBS) {
		value = grams_to_lbs(grams);
		unit = _("lbs");
		decimals = 0;
	} else {
		value = grams / 1000.0;
		unit = _("kg");
		decimals = 1;
	}
	if (frac)
		*frac = decimals;
	if (units)
		*units = unit;
	return value;
}

struct dive *alloc_dive(void)
{
	struct dive *dive;

	dive = malloc(sizeof(*dive));
	if (!dive)
		exit(1);
	memset(dive, 0, sizeof(*dive));
	return dive;
}

struct sample *prepare_sample(struct divecomputer *dc)
{
	if (dc) {
		int nr = dc->samples;
		int alloc_samples = dc->alloc_samples;
		struct sample *sample;
		if (nr >= alloc_samples) {
			struct sample *newsamples;

			alloc_samples = (alloc_samples * 3)/2 + 10;
			newsamples = realloc(dc->sample, alloc_samples * sizeof(struct sample));
			if (!newsamples)
				return NULL;
			dc->alloc_samples = alloc_samples;
			dc->sample = newsamples;
		}
		sample = dc->sample + nr;
		memset(sample, 0, sizeof(*sample));
		return sample;
	}
	return NULL;
}

void finish_sample(struct divecomputer *dc)
{
	dc->samples++;
}

/*
 * So when we re-calculate maxdepth and meandepth, we will
 * not override the old numbers if they are close to the
 * new ones.
 *
 * Why? Because a dive computer may well actually track the
 * max depth and mean depth at finer granularity than the
 * samples it stores. So it's possible that the max and mean
 * have been reported more correctly originally.
 *
 * Only if the values calculated from the samples are clearly
 * different do we override the normal depth values.
 *
 * This considers 1m to be "clearly different". That's
 * a totally random number.
 */
static void update_depth(depth_t *depth, int new)
{
	if (new) {
		int old = depth->mm;

		if (abs(old - new) > 1000)
			depth->mm = new;
	}
}

static void update_duration(duration_t *duration, int new)
{
	if (new)
		duration->seconds = new;
}

static void update_temperature(temperature_t *temperature, int new)
{
	if (new) {
		int old = temperature->mkelvin;

		if (abs(old - new) > 1000)
			temperature->mkelvin = new;
	}
}

static void fixup_pressure(struct dive *dive, struct sample *sample)
{
	unsigned int pressure, index;
	cylinder_t *cyl;

	pressure = sample->cylinderpressure.mbar;
	if (!pressure)
		return;
	index = sample->cylinderindex;
	if (index >= MAX_CYLINDERS)
		return;
	cyl = dive->cylinder + index;
	if (!cyl->sample_start.mbar)
		cyl->sample_start.mbar = pressure;
	cyl->sample_end.mbar = pressure;
}

/*
 * If the cylinder tank pressures are within half a bar
 * (about 8 PSI) of the sample pressures, we consider it
 * to be a rounding error, and throw them away as redundant.
 */
static int same_rounded_pressure(pressure_t a, pressure_t b)
{
	return abs(a.mbar - b.mbar) <= 500;
}

static void sanitize_gasmix(struct gasmix *mix)
{
	unsigned int o2, he;

	o2 = mix->o2.permille;
	he = mix->he.permille;

	/* Regular air: leave empty */
	if (!he) {
		if (!o2)
			return;
		/* 20.9% or 21% O2 is just air */
		if (o2 >= 209 && o2 <= 210) {
			mix->o2.permille = 0;
			return;
		}
	}

	/* Sane mix? */
	if (o2 <= 1000 && he <= 1000 && o2+he <= 1000)
		return;
	fprintf(stderr, "Odd gasmix: %u O2 %u He\n", o2, he);
	memset(mix, 0, sizeof(*mix));
}

/*
 * See if the size/workingpressure looks like some standard cylinder
 * size, eg "AL80".
 */
static void match_standard_cylinder(cylinder_type_t *type)
{
	double cuft;
	int psi, len;
	const char *fmt;
	char buffer[20], *p;

	/* Do we already have a cylinder description? */
	if (type->description)
		return;

	cuft = ml_to_cuft(type->size.mliter);
	cuft *= to_ATM(type->workingpressure);
	psi = to_PSI(type->workingpressure);

	switch (psi) {
	case 2300 ... 2500:	/* 2400 psi: LP tank */
		fmt = "LP%d";
		break;
	case 2600 ... 2700:	/* 2640 psi: LP+10% */
		fmt = "LP%d";
		break;
	case 2900 ... 3100:	/* 3000 psi: ALx tank */
		fmt = "AL%d";
		break;
	case 3400 ... 3500:	/* 3442 psi: HP tank */
		fmt = "HP%d";
		break;
	case 3700 ... 3850:	/* HP+10% */
		fmt = "HP%d+";
		break;
	default:
		return;
	}
	len = snprintf(buffer, sizeof(buffer), fmt, (int) (cuft+0.5));
	p = malloc(len+1);
	if (!p)
		return;
	memcpy(p, buffer, len+1);
	type->description = p;
}


/*
 * There are two ways to give cylinder size information:
 *  - total amount of gas in cuft (depends on working pressure and physical size)
 *  - physical size
 *
 * where "physical size" is the one that actually matters and is sane.
 *
 * We internally use physical size only. But we save the workingpressure
 * so that we can do the conversion if required.
 */
static void sanitize_cylinder_type(cylinder_type_t *type)
{
	double volume_of_air, atm, volume;

	/* If we have no working pressure, it had *better* be just a physical size! */
	if (!type->workingpressure.mbar)
		return;

	/* No size either? Nothing to go on */
	if (!type->size.mliter)
		return;

	if (input_units.volume == CUFT) {
		/* confusing - we don't really start from ml but millicuft !*/
		volume_of_air = cuft_to_l(type->size.mliter);
		atm = to_ATM(type->workingpressure);		/* working pressure in atm */
		volume = volume_of_air / atm;			/* milliliters at 1 atm: "true size" */
		type->size.mliter = volume + 0.5;
	}

	/* Ok, we have both size and pressure: try to match a description */
	match_standard_cylinder(type);
}

static void sanitize_cylinder_info(struct dive *dive)
{
	int i;

	for (i = 0; i < MAX_CYLINDERS; i++) {
		sanitize_gasmix(&dive->cylinder[i].gasmix);
		sanitize_cylinder_type(&dive->cylinder[i].type);
	}
}

/* some events should never be thrown away */
static gboolean is_potentially_redundant(struct event *event)
{
	if (!strcmp(event->name, "gaschange"))
		return FALSE;
	if (!strcmp(event->name, "bookmark"))
		return FALSE;
	if (!strcmp(event->name, "heading"))
		return FALSE;
	return TRUE;
}

/* match just by name - we compare the details in the code that uses this helper */
static struct event *find_previous_event(struct divecomputer *dc, struct event *event)
{
	struct event *ev = dc->events;
	struct event *previous = NULL;

	if (!event->name)
		return NULL;
	while (ev && ev != event) {
		if(ev->name && !strcmp(ev->name, event->name))
			previous = ev;
		ev = ev->next;
	}
	return previous;
}

struct dive *fixup_dive(struct dive *dive)
{
	int i,j;
	double depthtime = 0;
	int lasttime = 0;
	int lastindex = -1;
	int start = -1, end = -1;
	int maxdepth = 0, mintemp = 0;
	int lastdepth = 0;
	int lasttemp = 0, lastpressure = 0;
	int pressure_delta[MAX_CYLINDERS] = {INT_MAX, };
	struct divecomputer *dc;
	struct event *event;

	add_people(dive->buddy);
	add_people(dive->divemaster);
	add_location(dive->location);
	add_suit(dive->suit);
	sanitize_cylinder_info(dive);
	dc = &dive->dc;
	for (i = 0; i < dc->samples; i++) {
		struct sample *sample = dc->sample + i;
		int time = sample->time.seconds;
		int depth = sample->depth.mm;
		int temp = sample->temperature.mkelvin;
		int pressure = sample->cylinderpressure.mbar;
		int index = sample->cylinderindex;

		if (index == lastindex) {
			/* Remove duplicate redundant pressure information */
			if (pressure == lastpressure)
				sample->cylinderpressure.mbar = 0;
			/* check for simply linear data in the samples
			   +INT_MAX means uninitialized, -INT_MAX means not linear */
			if (pressure_delta[index] != -INT_MAX && lastpressure) {
				if (pressure_delta[index] == INT_MAX) {
					pressure_delta[index] = abs(pressure - lastpressure);
				} else {
					int cur_delta = abs(pressure - lastpressure);
					if (cur_delta && abs(cur_delta - pressure_delta[index]) > 150) {
						/* ok the samples aren't just a linearisation
						 * between start and end */
						pressure_delta[index] = -INT_MAX;
					}
				}
			}
		}
		lastindex = index;
		lastpressure = pressure;

		if (lastdepth > SURFACE_THRESHOLD)
			end = time;

		if (depth > SURFACE_THRESHOLD) {
			if (start < 0)
				start = lasttime;
			if (depth > maxdepth)
				maxdepth = depth;
		}

		fixup_pressure(dive, sample);

		if (temp) {
			/*
			 * If we have consecutive identical
			 * temperature readings, throw away
			 * the redundant ones.
			 */
			if (lasttemp == temp)
				sample->temperature.mkelvin = 0;
			else
				lasttemp = temp;

			if (!mintemp || temp < mintemp)
				mintemp = temp;
		}
		depthtime += (time - lasttime) * (lastdepth + depth) / 2;
		lastdepth = depth;
		lasttime = time;
	}
	dive->start = start;
	dive->end = end;
	/* if all the samples for a cylinder have pressure data that
	 * is basically equidistant throw out the sample cylinder pressure
	 * information but make sure we still have a valid start and end
	 * pressure
	 * this happens when DivingLog decides to linearalize the
	 * pressure between beginning and end and for strange reasons
	 * decides to put that in the sample data as if it came from
	 * the dive computer; we don't want that (we'll visualize with
	 * constant SAC rate instead)
	 * WARNING WARNING - I have only seen this in single tank dives
	 * --- maybe I should try to create a multi tank dive and see what
	 * --- divinglog does there - but the code right now is only tested
	 * --- for the single tank case */
	for (j = 0; j < MAX_CYLINDERS; j++) {
		if (abs(pressure_delta[j]) != INT_MAX) {
			cylinder_t *cyl = dive->cylinder + j;
			for (i = 0; i < dc->samples; i++)
				if (dc->sample[i].cylinderindex == j)
					dc->sample[i].cylinderpressure.mbar = 0;
			if (! cyl->start.mbar)
				cyl->start.mbar = cyl->sample_start.mbar;
			if (! cyl->end.mbar)
				cyl->end.mbar = cyl->sample_end.mbar;
			cyl->sample_start.mbar = 0;
			cyl->sample_end.mbar = 0;
		}
	}
	if (end < 0) {
		/* Assume an ascent/descent rate of 9 m/min */
		int depth = dive->maxdepth.mm;
		int asc_desc_time = depth*60/9000;
		int duration = dive->duration.seconds;

		/* Some sanity checks against insane dives */
		if (duration < 2)
			duration = 2;
		if (asc_desc_time * 2 >= duration)
			asc_desc_time = duration/2;

		dive->meandepth.mm = depth*(duration-asc_desc_time)/duration;
		return dive;
	}

	update_duration(&dive->duration, end - start);
	if (start != end)
		depthtime /= (end - start);

	update_depth(&dive->meandepth, depthtime);
	update_temperature(&dive->watertemp, mintemp);
	update_depth(&dive->maxdepth, maxdepth);

	for (i = 0; i < MAX_CYLINDERS; i++) {
		cylinder_t *cyl = dive->cylinder + i;
		add_cylinder_description(&cyl->type);
		if (same_rounded_pressure(cyl->sample_start, cyl->start))
			cyl->start.mbar = 0;
		if (same_rounded_pressure(cyl->sample_end, cyl->end))
			cyl->end.mbar = 0;
	}
	for (i = 0; i < MAX_WEIGHTSYSTEMS; i++) {
		weightsystem_t *ws = dive->weightsystem + i;
		add_weightsystem_description(ws);
	}

	/* events are stored as a linked list, so the concept of
	 * "consecutive, identical events" is somewhat hard to
	 * implement correctly (especially given that on some dive
	 * computers events are asynchronous, so they can come in
	 * between what would be the non-constant sample rate).
	 *
	 * So what we do is that we throw away clearly redundant
	 * events that are fewer than 61 seconds apart (assuming there
	 * is no dive computer with a sample rate of more than 60
	 * seconds... that would be pretty pointless to plot the
	 * profile with)
	 * We first only mark the events for deletion so that we
	 * still know when the previous event happened. */
	event = dc->events;
	while (event) {
		struct event *prev;
		if (is_potentially_redundant(event)) {
			prev = find_previous_event(dc, event);
			if (prev && prev->value == event->value &&
			    prev->flags == event->flags &&
			    event->time.seconds - prev->time.seconds < 61)
				event->deleted = TRUE;
		}
		event = event->next;
	}
	event = dc->events;
	while (event) {
		if (event->next && event->next->deleted) {
			struct event *nextnext = event->next->next;
			free(event->next);
			event->next = nextnext;
		} else {
			event = event->next;
		}
	}

	return dive;
}

/* Don't pick a zero for MERGE_MIN() */
#define MERGE_MAX(res, a, b, n) res->n = MAX(a->n, b->n)
#define MERGE_MIN(res, a, b, n) res->n = (a->n)?(b->n)?MIN(a->n, b->n):(a->n):(b->n)
#define MERGE_TXT(res, a, b, n) res->n = merge_text(a->n, b->n)
#define MERGE_NONZERO(res, a, b, n) res->n = a->n ? a->n : b->n

#define MERGE_MAX_PREFDL(res, dl, a, b, n) res->n = (dl && dl->n) ? dl->n : MAX(a->n, b->n)
#define MERGE_MIN_PREFDL(res, dl, a, b, n) res->n = (dl && dl->n) ? dl->n : (a->n)?(b->n)?MIN(a->n, b->n):(a->n):(b->n)

static struct sample *add_sample(struct sample *sample, int time, struct divecomputer *dc)
{
	struct sample *p = prepare_sample(dc);

	if (p) {
		*p = *sample;
		p->time.seconds = time;
		finish_sample(dc);
	}
	return p;
}

/*
 * This is like add_sample(), but if the distance from the last sample
 * is excessive, we add two surface samples in between.
 *
 * This is so that if you merge two non-overlapping dives, we make sure
 * that the time in between the dives is at the surface, not some "last
 * sample that happened to be at a depth of 1.2m".
 */
static void merge_one_sample(struct sample *sample, int time, struct divecomputer *dc)
{
	int last = dc->samples-1;
	if (last >= 0) {
		static struct sample surface;
		int last_time = dc->sample[last].time.seconds;
		if (time > last_time + 60) {
			add_sample(&surface, last_time+20, dc);
			add_sample(&surface, time - 20, dc);
		}
	}
	add_sample(sample, time, dc);
}


/*
 * Merge samples. Dive 'a' is "offset" seconds before Dive 'b'
 */
static void merge_samples(struct divecomputer *res, struct divecomputer *a, struct divecomputer *b, int offset)
{
	int asamples = a->samples;
	int bsamples = b->samples;
	struct sample *as = a->sample;
	struct sample *bs = b->sample;

	/*
	 * We want a positive sample offset, so that sample
	 * times are always positive. So if the samples for
	 * 'b' are before the samples for 'a' (so the offset
	 * is negative), we switch a and b around, and use
	 * the reverse offset.
	 */
	if (offset < 0) {
		offset = -offset;
		asamples = bsamples;
		bsamples = a->samples;
		as = bs;
		bs = a->sample;
	}

	for (;;) {
		int at, bt;
		struct sample sample;

		if (!res)
			return;

		at = asamples ? as->time.seconds : -1;
		bt = bsamples ? bs->time.seconds + offset : -1;

		/* No samples? All done! */
		if (at < 0 && bt < 0)
			return;

		/* Only samples from a? */
		if (bt < 0) {
add_sample_a:
			merge_one_sample(as, at, res);
			as++;
			asamples--;
			continue;
		}

		/* Only samples from b? */
		if (at < 0) {
add_sample_b:
			merge_one_sample(bs, bt, res);
			bs++;
			bsamples--;
			continue;
		}

		if (at < bt)
			goto add_sample_a;
		if (at > bt)
			goto add_sample_b;

		/* same-time sample: add a merged sample. Take the non-zero ones */
		sample = *bs;
		if (as->depth.mm)
			sample.depth = as->depth;
		if (as->temperature.mkelvin)
			sample.temperature = as->temperature;
		if (as->cylinderpressure.mbar)
			sample.cylinderpressure = as->cylinderpressure;
		if (as->cylinderindex)
			sample.cylinderindex = as->cylinderindex;

		merge_one_sample(&sample, at, res);

		as++;
		bs++;
		asamples--;
		bsamples--;
	}
}

static char *merge_text(const char *a, const char *b)
{
	char *res;

	if (!a || !*a)
		return (char *)b;
	if (!b || !*b)
		return (char *)a;
	if (!strcmp(a,b))
		return (char *)a;
	res = malloc(strlen(a) + strlen(b) + 9);
	if (!res)
		return (char *)a;
	sprintf(res, _("(%s) or (%s)"), a, b);
	return res;
}

#define SORT(a,b,field) \
	if (a->field != b->field) return a->field < b->field ? -1 : 1

static int sort_event(struct event *a, struct event *b)
{
	SORT(a,b,time.seconds);
	SORT(a,b,type);
	SORT(a,b,flags);
	SORT(a,b,value);
	return strcmp(a->name, b->name);
}

static void merge_events(struct divecomputer *res, struct divecomputer *src1, struct divecomputer *src2, int offset)
{
	struct event *a, *b;
	struct event **p = &res->events;

	/* Always use positive offsets */
	if (offset < 0) {
		struct divecomputer *tmp;

		offset = -offset;
		tmp = src1;
		src1 = src2;
		src2 = tmp;
	}

	a = src1->events;
	b = src2->events;
	while (b) {
		b->time.seconds += offset;
		b = b->next;
	}
	b = src2->events;

	while (a || b) {
		int s;
		if (!b) {
			*p = a;
			break;
		}
		if (!a) {
			*p = b;
			break;
		}
		s = sort_event(a, b);
		/* Pick b */
		if (s > 0) {
			*p = b;
			p = &b->next;
			b = b->next;
			continue;
		}
		/* Pick 'a' or neither */
		if (s < 0) {
			*p = a;
			p = &a->next;
		}
		a = a->next;
		continue;
	}
}

/* Pick whichever has any info (if either). Prefer 'a' */
static void merge_cylinder_type(cylinder_type_t *res, cylinder_type_t *a, cylinder_type_t *b)
{
	if (a->size.mliter)
		b = a;
	*res = *b;
}

static void merge_cylinder_mix(struct gasmix *res, struct gasmix *a, struct gasmix *b)
{
	if (a->o2.permille)
		b = a;
	*res = *b;
}

static void merge_cylinder_info(cylinder_t *res, cylinder_t *a, cylinder_t *b)
{
	merge_cylinder_type(&res->type, &a->type, &b->type);
	merge_cylinder_mix(&res->gasmix, &a->gasmix, &b->gasmix);
	MERGE_MAX(res, a, b, start.mbar);
	MERGE_MIN(res, a, b, end.mbar);
}

static void merge_weightsystem_info(weightsystem_t *res, weightsystem_t *a, weightsystem_t *b)
{
	if (!a->weight.grams)
		a = b;
	*res = *a;
}

static void merge_equipment(struct dive *res, struct dive *a, struct dive *b)
{
	int i;

	for (i = 0; i < MAX_CYLINDERS; i++)
		merge_cylinder_info(res->cylinder+i, a->cylinder + i, b->cylinder + i);
	for (i = 0; i < MAX_WEIGHTSYSTEMS; i++)
		merge_weightsystem_info(res->weightsystem+i, a->weightsystem + i, b->weightsystem + i);
}

/*
 * When merging two dives, this picks the trip from one, and removes it
 * from the other.
 *
 * The 'next' dive is not involved in the dive merging, but is the dive
 * that will be the next dive after the merged dive.
 */
static void pick_trip(struct dive *res, struct dive *pick)
{
	tripflag_t tripflag = pick->tripflag;
	dive_trip_t *trip = pick->divetrip;

	res->tripflag = tripflag;
	add_dive_to_trip(res, trip);
}

/*
 * Pick a trip for a dive
 */
static void merge_trip(struct dive *res, struct dive *a, struct dive *b)
{
	dive_trip_t *atrip, *btrip;

	/*
	 * The larger tripflag is more relevant: we prefer
	 * take manually assigned trips over auto-generated
	 * ones.
	 */
	if (a->tripflag > b->tripflag)
		goto pick_a;

	if (a->tripflag < b->tripflag)
		goto pick_b;

	/* Otherwise, look at the trip data and pick the "better" one */
	atrip = a->divetrip;
	btrip = b->divetrip;
	if (!atrip)
		goto pick_b;
	if (!btrip)
		goto pick_a;
	if (!atrip->location)
		goto pick_b;
	if (!btrip->location)
		goto pick_a;
	if (!atrip->notes)
		goto pick_b;
	if (!btrip->notes)
		goto pick_a;

	/*
	 * Ok, so both have location and notes.
	 * Pick the earlier one.
	 */
	if (a->when < b->when)
		goto pick_a;
	goto pick_b;

pick_a:
	b = a;
pick_b:
	pick_trip(res, b);
}

#if CURRENTLY_NOT_USED
/*
 * Sample 's' is between samples 'a' and 'b'. It is 'offset' seconds before 'b'.
 *
 * If 's' and 'a' are at the same time, offset is 0, and b is NULL.
 */
static int compare_sample(struct sample *s, struct sample *a, struct sample *b, int offset)
{
	unsigned int depth = a->depth.mm;
	int diff;

	if (offset) {
		unsigned int interval = b->time.seconds - a->time.seconds;
		unsigned int depth_a = a->depth.mm;
		unsigned int depth_b = b->depth.mm;

		if (offset > interval)
			return -1;

		/* pick the average depth, scaled by the offset from 'b' */
		depth = (depth_a * offset) + (depth_b * (interval - offset));
		depth /= interval;
	}
	diff = s->depth.mm - depth;
	if (diff < 0)
		diff = -diff;
	/* cut off at one meter difference */
	if (diff > 1000)
		diff = 1000;
	return diff*diff;
}

/*
 * Calculate a "difference" in samples between the two dives, given
 * the offset in seconds between them. Use this to find the best
 * match of samples between two different dive computers.
 */
static unsigned long sample_difference(struct divecomputer *a, struct divecomputer *b, int offset)
{
	int asamples = a->samples;
	int bsamples = b->samples;
	struct sample *as = a->sample;
	struct sample *bs = b->sample;
	unsigned long error = 0;
	int start = -1;

	if (!asamples || !bsamples)
		return 0;

	/*
	 * skip the first sample - this way we know can always look at
	 * as/bs[-1] to look at the samples around it in the loop.
	 */
	as++; bs++;
	asamples--;
	bsamples--;

	for (;;) {
		int at, bt, diff;


		/* If we run out of samples, punt */
		if (!asamples)
			return INT_MAX;
		if (!bsamples)
			return INT_MAX;

		at = as->time.seconds;
		bt = bs->time.seconds + offset;

		/* b hasn't started yet? Ignore it */
		if (bt < 0) {
			bs++;
			bsamples--;
			continue;
		}

		if (at < bt) {
			diff = compare_sample(as, bs-1, bs, bt - at);
			as++;
			asamples--;
		} else if (at > bt) {
			diff = compare_sample(bs, as-1, as, at - bt);
			bs++;
			bsamples--;
		} else {
			diff = compare_sample(as, bs, NULL, 0);
			as++; bs++;
			asamples--; bsamples--;
		}

		/* Invalid comparison point? */
		if (diff < 0)
			continue;

		if (start < 0)
			start = at;

		error += diff;

		if (at - start > 120)
			break;
	}
	return error;
}

/*
 * Dive 'a' is 'offset' seconds before dive 'b'
 *
 * This is *not* because the dive computers clocks aren't in sync,
 * it is because the dive computers may "start" the dive at different
 * points in the dive, so the sample at time X in dive 'a' is the
 * same as the sample at time X+offset in dive 'b'.
 *
 * For example, some dive computers take longer to "wake up" when
 * they sense that you are under water (ie Uemis Zurich if it was off
 * when the dive started). And other dive computers have different
 * depths that they activate at, etc etc.
 *
 * If we cannot find a shared offset, don't try to merge.
 */
static int find_sample_offset(struct divecomputer *a, struct divecomputer *b)
{
	int offset, best;
	unsigned long max;

	/* No samples? Merge at any time (0 offset) */
	if (!a->samples)
		return 0;
	if (!b->samples)
		return 0;

	/*
	 * Common special-case: merging a dive that came from
	 * the same dive computer, so the samples are identical.
	 * Check this first, without wasting time trying to find
	 * some minimal offset case.
	 */
	best = 0;
	max = sample_difference(a, b, 0);
	if (!max)
		return 0;

	/*
	 * Otherwise, look if we can find anything better within
	 * a thirty second window..
	 */
	for (offset = -30; offset <= 30; offset++) {
		unsigned long diff;

		diff = sample_difference(a, b, offset);
		if (diff > max)
			continue;
		best = offset;
		max = diff;
	}

	return best;
}
#endif

/*
 * Are a and b "similar" values, when given a reasonable lower end expected
 * difference?
 *
 * So for example, we'd expect different dive computers to give different
 * max depth readings. You might have them on different arms, and they
 * have different pressure sensors and possibly different ideas about
 * water salinity etc.
 *
 * So have an expected minimum difference, but also allow a larger relative
 * error value.
 */
static int similar(unsigned long a, unsigned long b, unsigned long expected)
{
	if (a && b) {
		unsigned long min, max, diff;

		min = a; max = b;
		if (a > b) {
			min = b;
			max = a;
		}
		diff = max - min;

		/* Smaller than expected difference? */
		if (diff < expected)
			return 1;
		/* Error less than 10% or the maximum */
		if (diff*10 < max)
			return 1;
	}
	return 0;
}

static int same_dive_computer(struct dive *a, struct dive *b)
{
	/* No model info in one or the other? Assume they're the same */
	if (!a->dc.model || !b->dc.model)
		return 1;
	if (strcasecmp(a->dc.model, b->dc.model))
		return 0;
	/* No device ID? Assume same.. */
	if (!a->dc.deviceid || !b->dc.deviceid)
		return 1;
	return 0;
}

/*
 * Do we want to automatically try to merge two dives that
 * look like they are the same dive?
 *
 * This happens quite commonly because you download a dive
 * that you already had, or perhaps because you maintained
 * multiple dive logs and want to load them all together
 * (possibly one of them was imported from another dive log
 * application entirely).
 *
 * NOTE! We mainly look at the dive time, but it can differ
 * between two dives due to a few issues:
 *
 *  - rounding the dive date to the nearest minute in other dive
 *    applications
 *
 *  - dive computers with "relative datestamps" (ie the dive
 *    computer doesn't actually record an absolute date at all,
 *    but instead at download-time syncronizes its internal
 *    time with real-time on the downloading computer)
 *
 *  - using multiple dive computers with different real time on
 *    the same dive
 *
 * We do not merge dives that look radically different, and if
 * the dates are *too* far off the user will have to join two
 * dives together manually. But this tries to handle the sane
 * cases.
 */
static int likely_same_dive(struct dive *a, struct dive *b)
{
	int fuzz;

	/*
	 * Do some basic sanity testing of the values we
	 * have filled in during 'fixup_dive()'
	 */
	if (!similar(a->maxdepth.mm, b->maxdepth.mm, 1000) ||
	    !similar(a->meandepth.mm, b->meandepth.mm, 1000) ||
	    !similar(a->duration.seconds, b->duration.seconds, 5*60))
		return 0;

	/*
	 * Allow a minute difference by default (minute rounding etc),
	 * and more if the dive computers are clearly different.
	 */
	fuzz = same_dive_computer(a, b) ? 60 : 5*60;

	return ((a->when <= b->when + fuzz) && (a->when >= b->when - fuzz));
}

/*
 * This could do a lot more merging. Right now it really only
 * merges almost exact duplicates - something that happens easily
 * with overlapping dive downloads.
 */
struct dive *try_to_merge(struct dive *a, struct dive *b, gboolean prefer_downloaded)
{
	if (likely_same_dive(a, b))
		return merge_dives(a, b, 0, prefer_downloaded);
	return NULL;
}

static void free_events(struct event *ev)
{
	while (ev) {
		struct event *next = ev->next;
		free(ev);
		ev = next;
	}
}

static void free_dc(struct divecomputer *dc)
{
	free(dc->sample);
	free_events(dc->events);
	free(dc);
}

static int same_event(struct event *a, struct event *b)
{
	if (a->time.seconds != b->time.seconds)
		return 0;
	if (a->type != b->type)
		return 0;
	if (a->flags != b->flags)
		return 0;
	if (a->value != b->value)
		return 0;
	return !strcmp(a->name, b->name);
}

static int same_sample(struct sample *a, struct sample *b)
{
	if (a->time.seconds != b->time.seconds)
		return 0;
	if (a->depth.mm != b->depth.mm)
		return 0;
	if (a->temperature.mkelvin != b->temperature.mkelvin)
		return 0;
	if (a->cylinderpressure.mbar != b->cylinderpressure.mbar)
		return 0;
	return a->cylinderindex == b->cylinderindex;
}

static int same_dc(struct divecomputer *a, struct divecomputer *b)
{
	int i;
	struct event *eva, *evb;

	if (a->when && b->when && a->when != b->when)
		return 0;
	if (a->samples != b->samples)
		return 0;
	for (i = 0; i < a->samples; i++)
		if (!same_sample(a->sample+i, b->sample+i))
			return 0;
	eva = a->events;
	evb = b->events;
	while (eva && evb) {
		if (!same_event(eva, evb))
			return 0;
		eva = eva->next;
		evb = evb->next;
	}
	return eva == evb;
}

static void remove_redundant_dc(struct divecomputer *dc)
{
	do {
		struct divecomputer **p = &dc->next;

		/* Check this dc against all the following ones.. */
		while (*p) {
			struct divecomputer *check = *p;
			if (same_dc(dc, check)) {
				*p = check->next;
				check->next = NULL;
				free_dc(check);
				continue;
			}
			p = &check->next;
		}

		/* .. and then continue down the chain */
		dc = dc->next;
	} while (dc);
}

static void clear_dc(struct divecomputer *dc)
{
	memset(dc, 0, sizeof(*dc));
}

static struct divecomputer *find_matching_computer(struct divecomputer *match, struct divecomputer *list)
{
	struct divecomputer *p;

	while ((p = list) != NULL) {
		list = list->next;

		/* No dive computer model? That matches anything */
		if (!match->model || !p->model)
			break;

		/* Otherwise at least the model names have to match */
		if (strcasecmp(match->model, p->model))
			continue;

		/* No device ID? Match */
		if (!match->deviceid || !p->deviceid)
			break;

		if (match->deviceid == p->deviceid)
			break;
	}
	return p;
}

/*
 * Join dive computers with a specific time offset between
 * them.
 *
 * Use the dive computer ID's (or names, if ID's are missing)
 * to match them up. If we find a matching dive computer, we
 * merge them. If not, we just take the data from 'a'.
 */
static void interleave_dive_computers(struct divecomputer *res,
	struct divecomputer *a, struct divecomputer *b, int offset)
{
	do {
		struct divecomputer *match;

		res->model = a->model ? strdup(a->model) : NULL;
		res->deviceid = a->deviceid;
		res->diveid = a->diveid;
		res->next = NULL;

		match = find_matching_computer(a, b);
		if (match) {
			merge_events(res, a, match, offset);
			merge_samples(res, a, match, offset);
		} else {
			res->sample = a->sample;
			res->samples = a->samples;
			res->events = a->events;
			a->sample = NULL;
			a->samples = 0;
			a->events = NULL;
		}
		a = a->next;
		if (!a)
			break;
		res->next = calloc(1, sizeof(struct divecomputer));
		res = res->next;
	} while (res);
}


/*
 * Join dive computer information.
 *
 * If we have old-style dive computer information (no model
 * name etc), we will prefer a new-style one and just throw
 * away the old. We're assuming it's a re-download.
 *
 * Otherwise, we'll just try to keep all the information.
 */
static void join_dive_computers(struct divecomputer *res, struct divecomputer *a, struct divecomputer *b)
{
	if (a->model && !b->model) {
		*res = *a;
		clear_dc(a);
		return;
	}
	if (b->model && !a->model) {
		*res = *b;
		clear_dc(b);
		return;
	}

	*res = *a;
	clear_dc(a);
	while (res->next)
		res = res->next;

	res->next = calloc(1, sizeof(*res));
	*res->next = *b;
	clear_dc(b);

	remove_redundant_dc(res);
}

struct dive *merge_dives(struct dive *a, struct dive *b, int offset, gboolean prefer_downloaded)
{
	struct dive *res = alloc_dive();
	struct dive *dl = NULL;

	/* Aim for newly downloaded dives to be 'b' (keep old dive data first) */
	if (a->downloaded && !b->downloaded) {
		struct dive *tmp = a;
		a = b;
		b = tmp;
	}
	if (prefer_downloaded && b->downloaded)
		dl = b;

	res->when = dl ? dl->when : a->when;
	res->selected = a->selected || b->selected;
	merge_trip(res, a, b);
	MERGE_NONZERO(res, a, b, latitude.udeg);
	MERGE_NONZERO(res, a, b, longitude.udeg);
	MERGE_TXT(res, a, b, location);
	MERGE_TXT(res, a, b, notes);
	MERGE_TXT(res, a, b, buddy);
	MERGE_TXT(res, a, b, divemaster);
	MERGE_MAX(res, a, b, rating);
	MERGE_TXT(res, a, b, suit);
	MERGE_MAX(res, a, b, number);
	MERGE_MAX_PREFDL(res, dl, a, b, maxdepth.mm);
	res->meandepth.mm = 0;
	MERGE_NONZERO(res, a, b, salinity);
	MERGE_NONZERO(res, a, b, visibility);
	MERGE_NONZERO(res, a, b, surface_pressure.mbar);
	MERGE_MAX_PREFDL(res, dl, a, b, duration.seconds);
	MERGE_MAX_PREFDL(res, dl, a, b, surfacetime.seconds);
	MERGE_MAX_PREFDL(res, dl, a, b, airtemp.mkelvin);
	MERGE_MIN_PREFDL(res, dl, a, b, watertemp.mkelvin);
	merge_equipment(res, a, b);
	if (dl) {
		res->dc = dl->dc;
		/*
		 * Since we copied the events and samples,
		 * we can't free them from the source when
		 * we free it - so make sure the source
		 * dive computer data is cleared out.
		 */
		clear_dc(&dl->dc);
	} else if (offset)
		interleave_dive_computers(&res->dc, &a->dc, &b->dc, offset);
	else
		join_dive_computers(&res->dc, &a->dc, &b->dc);

	fixup_dive(res);
	return res;
}
