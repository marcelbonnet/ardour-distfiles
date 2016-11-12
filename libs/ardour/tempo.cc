/*
    Copyright (C) 2000-2002 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <algorithm>
#include <stdexcept>
#include <cmath>

#include <unistd.h>

#include <glibmm/threads.h>

#include "pbd/enumwriter.h"
#include "pbd/xml++.h"
#include "evoral/Beats.hpp"

#include "ardour/debug.h"
#include "ardour/lmath.h"
#include "ardour/tempo.h"

#include "pbd/i18n.h"
#include <locale.h>

using namespace std;
using namespace ARDOUR;
using namespace PBD;

using Timecode::BBT_Time;

/* _default tempo is 4/4 qtr=120 */

Meter    TempoMap::_default_meter (4.0, 4.0);
Tempo    TempoMap::_default_tempo (120.0);

/***********************************************************************/

double
Meter::frames_per_grid (const Tempo& tempo, framecnt_t sr) const
{
	/* This is tempo- and meter-sensitive. The number it returns
	   is based on the interval between any two lines in the
	   grid that is constructed from tempo and meter sections.

	   The return value IS NOT interpretable in terms of "beats".
	*/

	return (60.0 * sr) / (tempo.beats_per_minute() * (_note_type/tempo.note_type()));
}

double
Meter::frames_per_bar (const Tempo& tempo, framecnt_t sr) const
{
	return frames_per_grid (tempo, sr) * _divisions_per_bar;
}

/***********************************************************************/

const string TempoSection::xml_state_node_name = "Tempo";

TempoSection::TempoSection (const XMLNode& node)
	: MetricSection (0.0, 0, MusicTime, true)
	, Tempo (TempoMap::default_tempo())
	, _c_func (0.0)
	, _active (true)
	, _locked_to_meter (false)
{
	XMLProperty const * prop;
	LocaleGuard lg;
	BBT_Time bbt;
	double pulse;
	uint32_t frame;

	_legacy_bbt = BBT_Time (0, 0, 0);

	if ((prop = node.property ("start")) != 0) {
		if (sscanf (prop->value().c_str(), "%" PRIu32 "|%" PRIu32 "|%" PRIu32,
			    &bbt.bars,
			    &bbt.beats,
			    &bbt.ticks) == 3) {
			/* legacy session - start used to be in bbt*/
			_legacy_bbt = bbt;
			pulse = -1.0;
			info << _("Legacy session detected. TempoSection XML node will be altered.") << endmsg;
		}
	}

	if ((prop = node.property ("pulse")) != 0) {
		if (sscanf (prop->value().c_str(), "%lf", &pulse) != 1) {
			error << _("TempoSection XML node has an illegal \"pulse\" value") << endmsg;
		}
	}

	set_pulse (pulse);

	if ((prop = node.property ("frame")) != 0) {
		if (sscanf (prop->value().c_str(), "%" PRIu32, &frame) != 1) {
			error << _("TempoSection XML node has an illegal \"frame\" value") << endmsg;
		} else {
			set_frame (frame);
		}
	}

	if ((prop = node.property ("beats-per-minute")) == 0) {
		error << _("TempoSection XML node has no \"beats-per-minute\" property") << endmsg;
		throw failed_constructor();
	}

	if (sscanf (prop->value().c_str(), "%lf", &_beats_per_minute) != 1 || _beats_per_minute < 0.0) {
		error << _("TempoSection XML node has an illegal \"beats_per_minute\" value") << endmsg;
		throw failed_constructor();
	}

	if ((prop = node.property ("note-type")) == 0) {
		/* older session, make note type be quarter by default */
		_note_type = 4.0;
	} else {
		if (sscanf (prop->value().c_str(), "%lf", &_note_type) != 1 || _note_type < 1.0) {
			error << _("TempoSection XML node has an illegal \"note-type\" value") << endmsg;
			throw failed_constructor();
		}
	}

	if ((prop = node.property ("movable")) == 0) {
		error << _("TempoSection XML node has no \"movable\" property") << endmsg;
		throw failed_constructor();
	}

	set_movable (string_is_affirmative (prop->value()));

	if ((prop = node.property ("active")) == 0) {
		warning << _("TempoSection XML node has no \"active\" property") << endmsg;
		set_active(true);
	} else {
		set_active (string_is_affirmative (prop->value()));
	}

	if ((prop = node.property ("tempo-type")) == 0) {
		_type = Constant;
	} else {
		_type = Type (string_2_enum (prop->value(), _type));
	}

	if ((prop = node.property ("lock-style")) == 0) {
		if (movable()) {
			set_position_lock_style (MusicTime);
		} else {
			set_position_lock_style (AudioTime);
		}
	} else {
		set_position_lock_style (PositionLockStyle (string_2_enum (prop->value(), position_lock_style())));
	}

	if ((prop = node.property ("locked-to-meter")) == 0) {
		set_locked_to_meter (false);
	} else {
		set_locked_to_meter (string_is_affirmative (prop->value()));
	}
}

XMLNode&
TempoSection::get_state() const
{
	XMLNode *root = new XMLNode (xml_state_node_name);
	char buf[256];
	LocaleGuard lg;

	snprintf (buf, sizeof (buf), "%lf", pulse());
	root->add_property ("pulse", buf);
	snprintf (buf, sizeof (buf), "%li", frame());
	root->add_property ("frame", buf);
	snprintf (buf, sizeof (buf), "%lf", _beats_per_minute);
	root->add_property ("beats-per-minute", buf);
	snprintf (buf, sizeof (buf), "%lf", _note_type);
	root->add_property ("note-type", buf);
	snprintf (buf, sizeof (buf), "%s", movable()?"yes":"no");
	root->add_property ("movable", buf);
	snprintf (buf, sizeof (buf), "%s", active()?"yes":"no");
	root->add_property ("active", buf);
	root->add_property ("tempo-type", enum_2_string (_type));
	root->add_property ("lock-style", enum_2_string (position_lock_style()));
	root->add_property ("locked-to-meter", locked_to_meter()?"yes":"no");

	return *root;
}

void
TempoSection::set_type (Type type)
{
	_type = type;
}

/** returns the tempo in whole pulses per minute at the zero-based (relative to session) frame.
*/
double
TempoSection::tempo_at_frame (const framepos_t& f, const framecnt_t& frame_rate) const
{

	if (_type == Constant || _c_func == 0.0) {
		return pulses_per_minute();
	}

	return pulse_tempo_at_time (frame_to_minute (f - frame(), frame_rate));
}

/** returns the zero-based frame (relative to session)
   where the tempo in whole pulses per minute occurs in this section.
   pulse p is only used for constant tempos.
   note that the tempo map may have multiple such values.
*/
framepos_t
TempoSection::frame_at_tempo (const double& ppm, const double& p, const framecnt_t& frame_rate) const
{
	if (_type == Constant || _c_func == 0.0) {
		return ((p - pulse()) * frames_per_pulse (frame_rate))  + frame();
	}

	return minute_to_frame (time_at_pulse_tempo (ppm), frame_rate) + frame();
}
/** returns the tempo in pulses per minute at the zero-based (relative to session) beat.
*/
double
TempoSection::tempo_at_pulse (const double& p) const
{

	if (_type == Constant || _c_func == 0.0) {
		return pulses_per_minute();
	}
	double const ppm = pulse_tempo_at_pulse (p - pulse());
	return ppm;
}

/** returns the zero-based beat (relative to session)
   where the tempo in whole pulses per minute occurs given frame f. frame f is only used for constant tempos.
   note that the session tempo map may have multiple beats at a given tempo.
*/
double
TempoSection::pulse_at_tempo (const double& ppm, const framepos_t& f, const framecnt_t& frame_rate) const
{
	if (_type == Constant || _c_func == 0.0) {
		double const pulses = ((f - frame()) / frames_per_pulse (frame_rate)) + pulse();
		return  pulses;
	}
	return pulse_at_pulse_tempo (ppm) + pulse();
}

/** returns the zero-based pulse (relative to session origin)
   where the zero-based frame (relative to session)
   lies.
*/
double
TempoSection::pulse_at_frame (const framepos_t& f, const framecnt_t& frame_rate) const
{
	if (_type == Constant || _c_func == 0.0) {
		return ((f - frame()) / frames_per_pulse (frame_rate)) + pulse();
	}

	return pulse_at_time (frame_to_minute (f - frame(), frame_rate)) + pulse();
}

/** returns the zero-based frame (relative to session start frame)
   where the zero-based pulse (relative to session start)
   falls.
*/

framepos_t
TempoSection::frame_at_pulse (const double& p, const framecnt_t& frame_rate) const
{
	if (_type == Constant || _c_func == 0.0) {
		return (framepos_t) floor ((p - pulse()) * frames_per_pulse (frame_rate)) + frame();
	}

	return minute_to_frame (time_at_pulse (p - pulse()), frame_rate) + frame();
}

/*
Ramp Overview

      |                     *
Tempo |                   *
Tt----|-----------------*|
Ta----|--------------|*  |
      |            * |   |
      |         *    |   |
      |     *        |   |
T0----|*             |   |
  *   |              |   |
      _______________|___|____
      time           a   t (next tempo)
      [        c         ] defines c

Duration in beats at time a is the integral of some Tempo function.
In our case, the Tempo function (Tempo at time t) is
T(t) = T0(e^(ct))

with function constant
c = log(Ta/T0)/a
so
a = log(Ta/T0)/c

The integral over t of our Tempo function (the beat function, which is the duration in beats at some time t) is:
b(t) = T0(e^(ct) - 1) / c

To find the time t at beat duration b, we use the inverse function of the beat function (the time function) which can be shown to be:
t(b) = log((c.b / T0) + 1) / c

The time t at which Tempo T occurs is a as above:
t(T) = log(T / T0) / c

The beat at which a Tempo T occurs is:
b(T) = (T - T0) / c

The Tempo at which beat b occurs is:
T(b) = b.c + T0

We define c for this tempo ramp by placing a new tempo section at some time t after this one.
Our problem is that we usually don't know t.
We almost always know the duration in beats between this and the new section, so we need to find c in terms of the beat function.
Where a = t (i.e. when a is equal to the time of the next tempo section), the beat function reveals:
t = b log (Ta / T0) / (T0 (e^(log (Ta / T0)) - 1))

By substituting our expanded t as a in the c function above, our problem is reduced to:
c = T0 (e^(log (Ta / T0)) - 1) / b

Of course the word 'beat' has been left loosely defined above.
In music, a beat is defined by the musical pulse (which comes from the tempo)
and the meter in use at a particular time (how many  pulse divisions there are in one bar).
It would be more accurate to substitute the work 'pulse' for 'beat' above.

Anyway ...

We can now store c for future time calculations.
If the following tempo section (the one that defines c in conjunction with this one)
is changed or moved, c is no longer valid.

The public methods are session-relative.

Most of this stuff is taken from this paper:

WHERE’S THE BEAT?
TOOLS FOR DYNAMIC TEMPO CALCULATIONS
Jan C. Schacher
Martin Neukom
Zurich University of Arts
Institute for Computer Music and Sound Technology

https://www.zhdk.ch/fileadmin/data_subsites/data_icst/Downloads/Timegrid/ICST_Tempopolyphony_ICMC07.pdf

*/

/*
  compute this ramp's function constant using the end tempo (in whole pulses per minute)
  and duration (pulses into global start) of some later tempo section.
*/
double
TempoSection::compute_c_func_pulse (const double& end_bpm, const double& end_pulse, const framecnt_t& frame_rate)
{
	double const log_tempo_ratio = log (end_bpm / pulses_per_minute());
	return pulses_per_minute() *  (expm1 (log_tempo_ratio)) / (end_pulse - pulse());
}

/* compute the function constant from some later tempo section, given tempo (whole pulses/min.) and distance (in frames) from session origin */
double
TempoSection::compute_c_func_frame (const double& end_bpm, const framepos_t& end_frame, const framecnt_t& frame_rate) const
{
	return c_func (end_bpm, frame_to_minute (end_frame - frame(), frame_rate));
}

framepos_t
TempoSection::minute_to_frame (const double& time, const framecnt_t& frame_rate) const
{
	return (framepos_t) floor ((time * 60.0 * frame_rate) + 0.5);
}

double
TempoSection::frame_to_minute (const framepos_t& frame, const framecnt_t& frame_rate) const
{
	return (frame / (double) frame_rate) / 60.0;
}

/* position function */
double
TempoSection::a_func (double end_ppm, double c_func) const
{
	return log (end_ppm / pulses_per_minute()) /  c_func;
}

/*function constant*/
double
TempoSection::c_func (double end_ppm, double end_time) const
{
	return log (end_ppm / pulses_per_minute()) /  end_time;
}

/* tempo in ppm at time in minutes */
double
TempoSection::pulse_tempo_at_time (const double& time) const
{
	return exp (_c_func * time) * pulses_per_minute();
}

/* time in minutes at tempo in ppm */
double
TempoSection::time_at_pulse_tempo (const double& pulse_tempo) const
{
	return log (pulse_tempo / pulses_per_minute()) / _c_func;
}

/* tick at tempo in ppm */
double
TempoSection::pulse_at_pulse_tempo (const double& pulse_tempo) const
{
	return (pulse_tempo - pulses_per_minute()) / _c_func;
}

/* tempo in ppm at tick */
double
TempoSection::pulse_tempo_at_pulse (const double& pulse) const
{
	return (pulse * _c_func) + pulses_per_minute();
}

/* pulse at time in minutes */
double
TempoSection::pulse_at_time (const double& time) const
{
	return expm1 (_c_func * time) * (pulses_per_minute() / _c_func);
}

/* time in minutes at pulse */
double
TempoSection::time_at_pulse (const double& pulse) const
{
	return log1p ((_c_func * pulse) / pulses_per_minute()) / _c_func;
}

/***********************************************************************/

const string MeterSection::xml_state_node_name = "Meter";

MeterSection::MeterSection (const XMLNode& node)
	: MetricSection (0.0, 0, MusicTime, false), Meter (TempoMap::default_meter())
{
	XMLProperty const * prop;
	LocaleGuard lg;
	BBT_Time bbt;
	double pulse = 0.0;
	double beat = 0.0;
	framepos_t frame = 0;
	pair<double, BBT_Time> start;

	if ((prop = node.property ("start")) != 0) {
		if (sscanf (prop->value().c_str(), "%" PRIu32 "|%" PRIu32 "|%" PRIu32,
		    &bbt.bars,
		    &bbt.beats,
		    &bbt.ticks) < 3) {
			error << _("MeterSection XML node has an illegal \"start\" value") << endmsg;
		} else {
			/* legacy session - start used to be in bbt*/
			info << _("Legacy session detected - MeterSection XML node will be altered.") << endmsg;
			pulse = -1.0;
		}
	}

	if ((prop = node.property ("pulse")) != 0) {
		if (sscanf (prop->value().c_str(), "%lf", &pulse) != 1) {
			error << _("MeterSection XML node has an illegal \"pulse\" value") << endmsg;
		}
	}
	set_pulse (pulse);

	if ((prop = node.property ("beat")) != 0) {
		if (sscanf (prop->value().c_str(), "%lf", &beat) != 1) {
			error << _("MeterSection XML node has an illegal \"beat\" value") << endmsg;
		}
	}

	start.first = beat;

	if ((prop = node.property ("bbt")) == 0) {
		warning << _("MeterSection XML node has no \"bbt\" property") << endmsg;
	} else if (sscanf (prop->value().c_str(), "%" PRIu32 "|%" PRIu32 "|%" PRIu32,
		    &bbt.bars,
		    &bbt.beats,
		    &bbt.ticks) < 3) {
		error << _("MeterSection XML node has an illegal \"bbt\" value") << endmsg;
		throw failed_constructor();
	}

	start.second = bbt;
	set_beat (start);

	if ((prop = node.property ("frame")) != 0) {
		if (sscanf (prop->value().c_str(), "%li", &frame) != 1) {
			error << _("MeterSection XML node has an illegal \"frame\" value") << endmsg;
		} else {
			set_frame (frame);
		}
	}

	/* beats-per-bar is old; divisions-per-bar is new */

	if ((prop = node.property ("divisions-per-bar")) == 0) {
		if ((prop = node.property ("beats-per-bar")) == 0) {
			error << _("MeterSection XML node has no \"beats-per-bar\" or \"divisions-per-bar\" property") << endmsg;
			throw failed_constructor();
		}
	}
	if (sscanf (prop->value().c_str(), "%lf", &_divisions_per_bar) != 1 || _divisions_per_bar < 0.0) {
		error << _("MeterSection XML node has an illegal \"divisions-per-bar\" value") << endmsg;
		throw failed_constructor();
	}

	if ((prop = node.property ("note-type")) == 0) {
		error << _("MeterSection XML node has no \"note-type\" property") << endmsg;
		throw failed_constructor();
	}
	if (sscanf (prop->value().c_str(), "%lf", &_note_type) != 1 || _note_type < 0.0) {
		error << _("MeterSection XML node has an illegal \"note-type\" value") << endmsg;
		throw failed_constructor();
	}

	if ((prop = node.property ("movable")) == 0) {
		error << _("MeterSection XML node has no \"movable\" property") << endmsg;
		throw failed_constructor();
	}

	set_movable (string_is_affirmative (prop->value()));

	if ((prop = node.property ("lock-style")) == 0) {
		warning << _("MeterSection XML node has no \"lock-style\" property") << endmsg;
		if (movable()) {
			set_position_lock_style (MusicTime);
		} else {
			set_position_lock_style (AudioTime);
		}
	} else {
		set_position_lock_style (PositionLockStyle (string_2_enum (prop->value(), position_lock_style())));
	}
}

XMLNode&
MeterSection::get_state() const
{
	XMLNode *root = new XMLNode (xml_state_node_name);
	char buf[256];
	LocaleGuard lg;

	snprintf (buf, sizeof (buf), "%lf", pulse());
	root->add_property ("pulse", buf);
	snprintf (buf, sizeof (buf), "%" PRIu32 "|%" PRIu32 "|%" PRIu32,
		  bbt().bars,
		  bbt().beats,
		  bbt().ticks);
	root->add_property ("bbt", buf);
	snprintf (buf, sizeof (buf), "%lf", beat());
	root->add_property ("beat", buf);
	snprintf (buf, sizeof (buf), "%lf", _note_type);
	root->add_property ("note-type", buf);
	snprintf (buf, sizeof (buf), "%li", frame());
	root->add_property ("frame", buf);
	root->add_property ("lock-style", enum_2_string (position_lock_style()));
	snprintf (buf, sizeof (buf), "%lf", _divisions_per_bar);
	root->add_property ("divisions-per-bar", buf);
	snprintf (buf, sizeof (buf), "%s", movable()?"yes":"no");
	root->add_property ("movable", buf);

	return *root;
}

/***********************************************************************/
/*
  Tempo Map Overview

  The Shaggs - Things I Wonder
  https://www.youtube.com/watch?v=9wQK6zMJOoQ

  Tempo is the rate of the musical pulse.
  Meters divide the pulses into measures and beats.

  TempoSections - provide pulses in the form of beats_per_minute() and note_type() where note_type is the division of a whole pulse,
  and beats_per_minute is the number of note_types in one minute (unlike what its name suggests).
  Note that Tempo::beats_per_minute() has nothing to do with musical beats. It has been left that way because
  a shorter one hasn't been found yet (pulse_divisions_per_minute()?).

  MeterSecions - divide pulses into measures (via divisions_per_bar) and beats (via note_divisor).

  Both tempos and meters have a pulse position and a frame position.
  Meters also have a beat position, which is always 0.0 for the first meter.
  TempoSections and MeterSections may be locked to either audio or music (position lock style).
  The lock style determines the 'true' position of the section wich is used to calculate the other postion parameters of the section.

  The first tempo and first meter are special. they must move together, and must be locked to audio.
  Audio locked tempos which lie before the first meter are made inactive.
  They will be re-activated if the first meter is again placed before them.

  With tepo sections potentially being ramped, meters provide a way of mapping beats to whole pulses without
  referring to the tempo function(s) involved as the distance in whole pulses between a meter and a subsequent beat is
  sb->beat() - meter->beat() / meter->note_divisor().
  Because every meter falls on a known pulse, (derived from its bar), the rest is easy as the duration in pulses between
  two meters is of course
  (meater_b->bar - meter_a->bar) * meter_a->divisions_per_bar / meter_a->note_divisor.

  Below, beat calculations are based on meter sections and all pulse and tempo calculations are based on tempo sections.
  Beat to frame conversion of course requires the use of meter and tempo.

  Remembering that ramped tempo sections interact, it is important to avoid referring to any other tempos when moving tempo sections,
  Here, beats (meters) are used to determine the new pulse (see predict_tempo_position())

  Recomputing the map is the process where the 'missing' position
  (tempo pulse or meter pulse & beat in the case of AudioTime, frame for MusicTime) is calculated.
  We construct the tempo map by first using the frame or pulse position (depending on position lock style) of each tempo.
  We then use this tempo map (really just the tempos) to find the pulse or frame position of each meter (again depending on lock style).

  Having done this, we can now find any musical duration by selecting the tempo and meter covering the position (or tempo) in question
  and querying its appropriate meter/tempo.

  It is important to keep the _metrics in an order that makes sense.
  Because ramped MusicTime and AudioTime tempos can interact with each other,
  reordering is frequent. Care must be taken to keep _metrics in a solved state.
  Solved means ordered by frame or pulse with frame-accurate precision (see check_solved()).

  Music and Audio

  Music and audio-locked objects may seem interchangeable on the surface, but when translating
  between audio samples and beats, keep in mind that a sample is only a quantised approximation
  of the actual time (in minutes) of a beat.
  Thus if a gui user points to the frame occupying the start of a music-locked object on 1|3|0, it does not
  mean that this frame is the actual location in time of 1|3|0.

  You cannot use a frame measurement to determine beat distance except under special circumstances
  (e.g. where the user has requested that a beat lie on a SMPTE frame or if the tempo is known to be constant over the duration).

  This means is that a user operating on a musical grid must supply the desired beat position and/or current beat quantization in order for the
  sample space the user is operating at to be translated correctly to the object.

  The current approach is to interpret the supplied frame using the grid division the user has currently selected.
  If the user has no musical grid set, they are actually operating in sample space (even SMPTE frames are rounded to audio frame), so
  the supplied audio frame is interpreted as the desired musical location (beat_at_frame()).

  tldr: Beat, being a function of time, has nothing to do with sample rate, but time quantization can get in the way of precision.

  When frame_at_beat() is called, the position calculation is performed in pulses and minutes.
  The result is rounded to audio frames.
  When beat_at_frame() is called, the frame is converted to minutes, with no rounding performed on the result.

  So :
  frame_at_beat (beat_at_frame (frame)) == frame
  but :
  beat_at_frame (frame_at_beat (beat)) != beat due to the time quantization of frame_at_beat().

  Doing the second one will result in a beat distance error of up to 0.5 audio samples.
  So instead work in pulses and/or beats and only use beat position to caclulate frame position (e.g. after tempo change).
  For audio-locked objects, use frame position to calculate beat position.

  The above pointless example would then do:
  beat_at_pulse (pulse_at_beat (beat)) to avoid rounding.

*/
struct MetricSectionSorter {
    bool operator() (const MetricSection* a, const MetricSection* b) {
	    return a->pulse() < b->pulse();
    }
};

struct MetricSectionFrameSorter {
    bool operator() (const MetricSection* a, const MetricSection* b) {
	    return a->frame() < b->frame();
    }
};

TempoMap::TempoMap (framecnt_t fr)
{
	_frame_rate = fr;
	BBT_Time start (1, 1, 0);

	TempoSection *t = new TempoSection (0.0, 0, _default_tempo.beats_per_minute(), _default_tempo.note_type(), TempoSection::Ramp, AudioTime);
	MeterSection *m = new MeterSection (0.0, 0, 0.0, start, _default_meter.divisions_per_bar(), _default_meter.note_divisor(), AudioTime);

	t->set_movable (false);
	m->set_movable (false);

	/* note: frame time is correct (zero) for both of these */

	_metrics.push_back (t);
	_metrics.push_back (m);

}

TempoMap::~TempoMap ()
{
	Metrics::const_iterator d = _metrics.begin();
	while (d != _metrics.end()) {
		delete (*d);
		++d;
	}
	_metrics.clear();
}

void
TempoMap::remove_tempo (const TempoSection& tempo, bool complete_operation)
{
	bool removed = false;

	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		if ((removed = remove_tempo_locked (tempo))) {
			if (complete_operation) {
				recompute_map (_metrics);
			}
		}
	}

	if (removed && complete_operation) {
		PropertyChanged (PropertyChange ());
	}
}

bool
TempoMap::remove_tempo_locked (const TempoSection& tempo)
{
	Metrics::iterator i;

	for (i = _metrics.begin(); i != _metrics.end(); ++i) {
		if (dynamic_cast<TempoSection*> (*i) != 0) {
			if (tempo.frame() == (*i)->frame()) {
				if ((*i)->movable()) {
					delete (*i);
					_metrics.erase (i);
					return true;
				}
			}
		}
	}

	return false;
}

void
TempoMap::remove_meter (const MeterSection& tempo, bool complete_operation)
{
	bool removed = false;

	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		if ((removed = remove_meter_locked (tempo))) {
			if (complete_operation) {
				recompute_map (_metrics);
			}
		}
	}

	if (removed && complete_operation) {
		PropertyChanged (PropertyChange ());
	}
}

bool
TempoMap::remove_meter_locked (const MeterSection& meter)
{

	if (meter.position_lock_style() == AudioTime) {
		/* remove meter-locked tempo */
		for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
			TempoSection* t = 0;
			if ((t = dynamic_cast<TempoSection*> (*i)) != 0) {
				if (t->locked_to_meter() && meter.frame() == (*i)->frame()) {
					delete (*i);
					_metrics.erase (i);
					break;
				}
			}
		}
	}

	for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if (dynamic_cast<MeterSection*> (*i) != 0) {
			if (meter.frame() == (*i)->frame()) {
				if ((*i)->movable()) {
					delete (*i);
					_metrics.erase (i);
					return true;
				}
			}
		}
	}

	return false;
}

void
TempoMap::do_insert (MetricSection* section)
{
	bool need_add = true;
	/* we only allow new meters to be inserted on beat 1 of an existing
	 * measure.
	 */
	MeterSection* m = 0;
	if ((m = dynamic_cast<MeterSection*>(section)) != 0) {

		if ((m->bbt().beats != 1) || (m->bbt().ticks != 0)) {

			pair<double, BBT_Time> corrected = make_pair (m->beat(), m->bbt());
			corrected.second.beats = 1;
			corrected.second.ticks = 0;
			corrected.first = beat_at_bbt_locked (_metrics, corrected.second);
			warning << string_compose (_("Meter changes can only be positioned on the first beat of a bar. Moving from %1 to %2"),
						   m->bbt(), corrected.second) << endmsg;
			//m->set_pulse (corrected);
		}
	}

	/* Look for any existing MetricSection that is of the same type and
	   in the same bar as the new one, and remove it before adding
	   the new one. Note that this means that if we find a matching,
	   existing section, we can break out of the loop since we're
	   guaranteed that there is only one such match.
	*/

	for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {

		TempoSection* const tempo = dynamic_cast<TempoSection*> (*i);
		TempoSection* const insert_tempo = dynamic_cast<TempoSection*> (section);
		MeterSection* const meter = dynamic_cast<MeterSection*> (*i);
		MeterSection* const insert_meter = dynamic_cast<MeterSection*> (section);

		if (tempo && insert_tempo) {

			/* Tempo sections */
			bool const ipm = insert_tempo->position_lock_style() == MusicTime;
			if ((ipm && tempo->pulse() == insert_tempo->pulse()) || (!ipm && tempo->frame() == insert_tempo->frame())) {

				if (!tempo->movable()) {

					/* can't (re)move this section, so overwrite
					 * its data content (but not its properties as
					 * a section).
					 */

					*(dynamic_cast<Tempo*>(*i)) = *(dynamic_cast<Tempo*>(insert_tempo));
					(*i)->set_position_lock_style (AudioTime);
					TempoSection* t;
					if ((t = dynamic_cast<TempoSection*>(*i)) != 0) {
						t->set_type (insert_tempo->type());
					}
					need_add = false;
				} else {
					delete (*i);
					_metrics.erase (i);
				}
				break;
			}

		} else if (meter && insert_meter) {

			/* Meter Sections */

			bool const ipm = insert_meter->position_lock_style() == MusicTime;

			if ((ipm && meter->beat() == insert_meter->beat()) || (!ipm && meter->frame() == insert_meter->frame())) {

				if (!meter->movable()) {

					/* can't (re)move this section, so overwrite
					 * its data content (but not its properties as
					 * a section
					 */

					*(dynamic_cast<Meter*>(*i)) = *(dynamic_cast<Meter*>(insert_meter));
					(*i)->set_position_lock_style (AudioTime);
					need_add = false;
				} else {
					delete (*i);
					_metrics.erase (i);
				}

				break;
			}
		} else {
			/* non-matching types, so we don't care */
		}
	}

	/* Add the given MetricSection, if we didn't just reset an existing
	 * one above
	 */

	if (need_add) {
		MeterSection* const insert_meter = dynamic_cast<MeterSection*> (section);
		TempoSection* const insert_tempo = dynamic_cast<TempoSection*> (section);
		Metrics::iterator i;
		if (insert_meter) {
			for (i = _metrics.begin(); i != _metrics.end(); ++i) {
				MeterSection* const meter = dynamic_cast<MeterSection*> (*i);

				if (meter) {
					bool const ipm = insert_meter->position_lock_style() == MusicTime;
					if ((ipm && meter->beat() > insert_meter->beat()) || (!ipm && meter->frame() > insert_meter->frame())) {
						break;
					}
				}
			}
		} else if (insert_tempo) {
			for (i = _metrics.begin(); i != _metrics.end(); ++i) {
				TempoSection* const tempo = dynamic_cast<TempoSection*> (*i);

				if (tempo) {
					bool const ipm = insert_tempo->position_lock_style() == MusicTime;
					if ((ipm && tempo->pulse() > insert_tempo->pulse()) || (!ipm && tempo->frame() > insert_tempo->frame())) {
						break;
					}
				}
			}
		}

		_metrics.insert (i, section);
		//dump (_metrics, std::cout);
	}
}

TempoSection*
TempoMap::add_tempo (const Tempo& tempo, const double& pulse, const framepos_t& frame, ARDOUR::TempoSection::Type type, PositionLockStyle pls)
{
	TempoSection* ts = 0;
	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		ts = add_tempo_locked (tempo, pulse, frame, type, pls, true);
	}


	PropertyChanged (PropertyChange ());

	return ts;
}

void
TempoMap::replace_tempo (const TempoSection& ts, const Tempo& tempo, const double& pulse, const framepos_t& frame, TempoSection::Type type, PositionLockStyle pls)
{
	const bool locked_to_meter = ts.locked_to_meter();

	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		TempoSection& first (first_tempo());
		if (ts.frame() != first.frame()) {
			remove_tempo_locked (ts);
			add_tempo_locked (tempo, pulse, frame, type, pls, true, locked_to_meter);
		} else {
			first.set_type (type);
			first.set_pulse (0.0);
			first.set_frame (frame);
			first.set_position_lock_style (AudioTime);
			{
				/* cannot move the first tempo section */
				*static_cast<Tempo*>(&first) = tempo;
				recompute_map (_metrics);
			}
		}
	}

	PropertyChanged (PropertyChange ());
}

TempoSection*
TempoMap::add_tempo_locked (const Tempo& tempo, double pulse, framepos_t frame
			    , TempoSection::Type type, PositionLockStyle pls, bool recompute, bool locked_to_meter)
{
	TempoSection* t = new TempoSection (pulse, frame, tempo.beats_per_minute(), tempo.note_type(), type, pls);
	t->set_locked_to_meter (locked_to_meter);
	bool solved = false;

	do_insert (t);

	if (recompute) {
		if (pls == AudioTime) {
			solved = solve_map_frame (_metrics, t, t->frame());
		} else {
			solved = solve_map_pulse (_metrics, t, t->pulse());
		}
		recompute_meters (_metrics);
	}

	if (!solved && recompute) {
		recompute_map (_metrics);
	}

	return t;
}

MeterSection*
TempoMap::add_meter (const Meter& meter, const double& beat, const Timecode::BBT_Time& where, const framepos_t& frame, PositionLockStyle pls)
{
	MeterSection* m = 0;
	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		m = add_meter_locked (meter, beat, where, frame, pls, true);
	}


#ifndef NDEBUG
	if (DEBUG_ENABLED(DEBUG::TempoMap)) {
		dump (_metrics, std::cerr);
	}
#endif

	PropertyChanged (PropertyChange ());
	return m;
}

void
TempoMap::replace_meter (const MeterSection& ms, const Meter& meter, const BBT_Time& where, const framepos_t& frame, PositionLockStyle pls)
{
	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		const double beat = beat_at_bbt_locked (_metrics, where);

		if (ms.movable()) {
			remove_meter_locked (ms);
			add_meter_locked (meter, beat, where, frame, pls, true);
		} else {
			MeterSection& first (first_meter());
			TempoSection& first_t (first_tempo());
			/* cannot move the first meter section */
			*static_cast<Meter*>(&first) = meter;
			first.set_position_lock_style (AudioTime);
			first.set_pulse (0.0);
			first.set_frame (frame);
			pair<double, BBT_Time> beat = make_pair (0.0, BBT_Time (1, 1, 0));
			first.set_beat (beat);
			first_t.set_frame (first.frame());
			first_t.set_pulse (0.0);
			first_t.set_position_lock_style (AudioTime);
			recompute_map (_metrics);
		}
	}

	PropertyChanged (PropertyChange ());
}

MeterSection*
TempoMap::add_meter_locked (const Meter& meter, double beat, const BBT_Time& where, framepos_t frame, PositionLockStyle pls, bool recompute)
{
	const MeterSection& prev_m = meter_section_at_frame_locked  (_metrics, frame - 1);
	const double pulse = ((where.bars - prev_m.bbt().bars) * (prev_m.divisions_per_bar() / prev_m.note_divisor())) + prev_m.pulse();
	TempoSection* mlt = 0;

	if (pls == AudioTime) {
		/* add meter-locked tempo */
		mlt = add_tempo_locked (tempo_at_frame_locked (_metrics, frame), pulse,  frame, TempoSection::Ramp, AudioTime, true, true);

		if (!mlt) {
			return 0;
		}

	}

	MeterSection* new_meter = new MeterSection (pulse, frame, beat, where, meter.divisions_per_bar(), meter.note_divisor(), pls);
	bool solved = false;

	do_insert (new_meter);

	if (recompute) {

		if (pls == AudioTime) {
			solved = solve_map_frame (_metrics, new_meter, frame);
		} else {
			solved = solve_map_bbt (_metrics, new_meter, where);
			/* required due to resetting the pulse of meter-locked tempi above.
			   Arguably  solve_map_bbt() should use solve_map_pulse (_metrics, TempoSection) instead,
			   but afaict this cannot cause the map to be left unsolved (these tempi are all audio locked).
			*/
			recompute_map (_metrics);
		}
	}

	if (!solved && recompute) {
		/* if this has failed to solve, there is little we can do other than to ensure that
		   the new map is recalculated.
		*/
		warning << "Adding meter may have left the tempo map unsolved." << endmsg;
		recompute_map (_metrics);
	}

	return new_meter;
}

void
TempoMap::change_initial_tempo (double beats_per_minute, double note_type)
{
	Tempo newtempo (beats_per_minute, note_type);
	TempoSection* t;

	for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if ((t = dynamic_cast<TempoSection*> (*i)) != 0) {
			if (!t->active()) {
				continue;
			}
			{
				Glib::Threads::RWLock::WriterLock lm (lock);
				*((Tempo*) t) = newtempo;
				recompute_map (_metrics);
			}
			PropertyChanged (PropertyChange ());
			break;
		}
	}
}

void
TempoMap::change_existing_tempo_at (framepos_t where, double beats_per_minute, double note_type)
{
	Tempo newtempo (beats_per_minute, note_type);

	TempoSection* prev;
	TempoSection* first;
	Metrics::iterator i;

	/* find the TempoSection immediately preceding "where"
	 */

	for (first = 0, i = _metrics.begin(), prev = 0; i != _metrics.end(); ++i) {

		if ((*i)->frame() > where) {
			break;
		}

		TempoSection* t;

		if ((t = dynamic_cast<TempoSection*>(*i)) != 0) {
			if (!t->active()) {
				continue;
			}
			if (!first) {
				first = t;
			}
			prev = t;
		}
	}

	if (!prev) {
		if (!first) {
			error << string_compose (_("no tempo sections defined in tempo map - cannot change tempo @ %1"), where) << endmsg;
			return;
		}

		prev = first;
	}

	/* reset */

	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		/* cannot move the first tempo section */
		*((Tempo*)prev) = newtempo;
		recompute_map (_metrics);
	}

	PropertyChanged (PropertyChange ());
}

const MeterSection&
TempoMap::first_meter () const
{
	const MeterSection *m = 0;

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if ((m = dynamic_cast<const MeterSection *> (*i)) != 0) {
			return *m;
		}
	}

	fatal << _("programming error: no meter section in tempo map!") << endmsg;
	abort(); /*NOTREACHED*/
	return *m;
}

MeterSection&
TempoMap::first_meter ()
{
	MeterSection *m = 0;

	/* CALLER MUST HOLD LOCK */

	for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if ((m = dynamic_cast<MeterSection *> (*i)) != 0) {
			return *m;
		}
	}

	fatal << _("programming error: no tempo section in tempo map!") << endmsg;
	abort(); /*NOTREACHED*/
	return *m;
}

const TempoSection&
TempoMap::first_tempo () const
{
	const TempoSection *t = 0;

	/* CALLER MUST HOLD LOCK */

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if ((t = dynamic_cast<const TempoSection *> (*i)) != 0) {
			if (!t->active()) {
				continue;
			}
			if (!t->movable()) {
				return *t;
			}
		}
	}

	fatal << _("programming error: no tempo section in tempo map!") << endmsg;
	abort(); /*NOTREACHED*/
	return *t;
}

TempoSection&
TempoMap::first_tempo ()
{
	TempoSection *t = 0;

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if ((t = dynamic_cast<TempoSection *> (*i)) != 0) {
			if (!t->active()) {
				continue;
			}
			if (!t->movable()) {
				return *t;
			}
		}
	}

	fatal << _("programming error: no tempo section in tempo map!") << endmsg;
	abort(); /*NOTREACHED*/
	return *t;
}
void
TempoMap::recompute_tempi (Metrics& metrics)
{
	TempoSection* prev_t = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		TempoSection* t;

		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if (!t->movable()) {
				if (!prev_t) {
					t->set_pulse (0.0);
					prev_t = t;
					continue;
				}
			}
			if (prev_t) {
				if (t->position_lock_style() == AudioTime) {
					prev_t->set_c_func (prev_t->compute_c_func_frame (t->pulses_per_minute(), t->frame(), _frame_rate));
					if (!t->locked_to_meter()) {
						t->set_pulse (prev_t->pulse_at_tempo (t->pulses_per_minute(), t->frame(), _frame_rate));
					}

				} else {
					prev_t->set_c_func (prev_t->compute_c_func_pulse (t->pulses_per_minute(), t->pulse(), _frame_rate));
					t->set_frame (prev_t->frame_at_tempo (t->pulses_per_minute(), t->pulse(), _frame_rate));

				}
			}
			prev_t = t;
		}
	}
	prev_t->set_c_func (0.0);
}

/* tempos must be positioned correctly.
   the current approach is to use a meter's bbt time as its base position unit.
   an audio-locked meter requires a recomputation of pulse and beat (but not bbt),
   while a music-locked meter requires recomputations of frame pulse and beat (but not bbt)
*/
void
TempoMap::recompute_meters (Metrics& metrics)
{
	MeterSection* meter = 0;
	MeterSection* prev_m = 0;

	for (Metrics::const_iterator mi = metrics.begin(); mi != metrics.end(); ++mi) {
		if (!(*mi)->is_tempo()) {
			meter = static_cast<MeterSection*> (*mi);
			if (meter->position_lock_style() == AudioTime) {
				double pulse = 0.0;
				pair<double, BBT_Time> b_bbt;
				TempoSection* meter_locked_tempo = 0;
				for (Metrics::const_iterator ii = metrics.begin(); ii != metrics.end(); ++ii) {
					TempoSection* t;
					if ((*ii)->is_tempo()) {
						t = static_cast<TempoSection*> (*ii);
						if ((t->locked_to_meter() || !t->movable()) && t->frame() == meter->frame()) {
							meter_locked_tempo = t;
							break;
						}
					}
				}

				if (prev_m) {
					const double beats = (meter->bbt().bars - prev_m->bbt().bars) * prev_m->divisions_per_bar();
					if (beats + prev_m->beat() != meter->beat()) {
						/* reordering caused a bbt change */
						b_bbt = make_pair (beats + prev_m->beat()
								   , BBT_Time ((beats / prev_m->divisions_per_bar()) + prev_m->bbt().bars, 1, 0));
						pulse = prev_m->pulse() + (beats / prev_m->note_divisor());

					} else if (meter->movable()) {
						b_bbt = make_pair (meter->beat(), meter->bbt());
						pulse = prev_m->pulse() + (beats / prev_m->note_divisor());
					}
				} else {
					b_bbt = make_pair (0.0, BBT_Time (1, 1, 0));
				}
				if (meter_locked_tempo) {
					meter_locked_tempo->set_pulse (pulse);
				}
				meter->set_beat (b_bbt);
				meter->set_pulse (pulse);

			} else {
				/* MusicTime */
				double pulse = 0.0;
				pair<double, BBT_Time> b_bbt;
				if (prev_m) {
					const double beats = (meter->bbt().bars - prev_m->bbt().bars) * prev_m->divisions_per_bar();
					if (beats + prev_m->beat() != meter->beat()) {
						/* reordering caused a bbt change */
						b_bbt = make_pair (beats + prev_m->beat()
								   , BBT_Time ((beats / prev_m->divisions_per_bar()) + prev_m->bbt().bars, 1, 0));
					} else {
						b_bbt = make_pair (beats + prev_m->beat(), meter->bbt());
					}
					pulse = (beats / prev_m->note_divisor()) + prev_m->pulse();
				} else {
					/* shouldn't happen - the first is audio-locked */
					pulse = pulse_at_beat_locked (metrics, meter->beat());
					b_bbt = make_pair (meter->beat(), meter->bbt());
				}

				meter->set_beat (b_bbt);
				meter->set_pulse (pulse);
				meter->set_frame (frame_at_pulse_locked (metrics, pulse));
			}

			prev_m = meter;
		}
	}
}

void
TempoMap::recompute_map (Metrics& metrics, framepos_t end)
{
	/* CALLER MUST HOLD WRITE LOCK */

	if (end < 0) {

		/* we will actually stop once we hit
		   the last metric.
		*/
		end = max_framepos;

	}

	DEBUG_TRACE (DEBUG::TempoMath, string_compose ("recomputing tempo map, zero to %1\n", end));

	if (end == 0) {
		/* silly call from Session::process() during startup
		 */
		return;
	}

	recompute_tempi (metrics);
	recompute_meters (metrics);
}

TempoMetric
TempoMap::metric_at (framepos_t frame, Metrics::const_iterator* last) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	TempoMetric m (first_meter(), first_tempo());

	/* at this point, we are *guaranteed* to have m.meter and m.tempo pointing
	   at something, because we insert the default tempo and meter during
	   TempoMap construction.

	   now see if we can find better candidates.
	*/

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {

		if ((*i)->frame() > frame) {
			break;
		}

		m.set_metric(*i);

		if (last) {
			*last = i;
		}
	}

	return m;
}

/* XX meters only */
TempoMetric
TempoMap::metric_at (BBT_Time bbt) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	TempoMetric m (first_meter(), first_tempo());

	/* at this point, we are *guaranteed* to have m.meter and m.tempo pointing
	   at something, because we insert the default tempo and meter during
	   TempoMap construction.

	   now see if we can find better candidates.
	*/

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		MeterSection* mw;
		if (!(*i)->is_tempo()) {
			mw = static_cast<MeterSection*> (*i);
			BBT_Time section_start (mw->bbt());

			if (section_start.bars > bbt.bars || (section_start.bars == bbt.bars && section_start.beats > bbt.beats)) {
				break;
			}

			m.set_metric (*i);
		}
	}

	return m;
}

/** Returns the beat duration corresponding to the supplied frame, possibly returning a negative value.
 * @param frame The session frame position.
 * @return The beat duration according to the tempo map at the supplied frame.
 * If the supplied frame lies before the first meter, the returned beat duration will be negative.
 * The returned beat is obtained using the first meter and the continuation of the tempo curve (backwards).
 *
 * This function uses both tempo and meter.
 */
double
TempoMap::beat_at_frame (const framecnt_t& frame) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return beat_at_frame_locked (_metrics, frame);
}

/* This function uses both tempo and meter.*/
double
TempoMap::beat_at_frame_locked (const Metrics& metrics, const framecnt_t& frame) const
{
	const TempoSection& ts = tempo_section_at_frame_locked (metrics, frame);
	MeterSection* prev_m = 0;
	MeterSection* next_m = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if (!(*i)->is_tempo()) {
			if (prev_m && (*i)->frame() > frame) {
				next_m = static_cast<MeterSection*> (*i);
				break;
			}
			prev_m = static_cast<MeterSection*> (*i);
		}
	}

	const double beat = prev_m->beat() + (ts.pulse_at_frame (frame, _frame_rate) - prev_m->pulse()) * prev_m->note_divisor();

	/* audio locked meters fake their beat */
	if (next_m && next_m->beat() < beat) {
		return next_m->beat();
	}

	return beat;
}

framepos_t
TempoMap::frame_at_beat (const double& beat) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return frame_at_beat_locked (_metrics, beat);
}

/* meter & tempo section based */
framepos_t
TempoMap::frame_at_beat_locked (const Metrics& metrics, const double& beat) const
{
	MeterSection* prev_m = 0;
	TempoSection* prev_t = 0;

	MeterSection* m;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m && m->beat() > beat) {
				break;
			}
			prev_m = m;
		}
	}

	TempoSection* t;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (prev_t && ((t->pulse() - prev_m->pulse()) * prev_m->note_divisor()) + prev_m->beat() > beat) {
				break;
			}
			prev_t = t;
		}

	}

	return prev_t->frame_at_pulse (((beat - prev_m->beat()) / prev_m->note_divisor()) + prev_m->pulse(), _frame_rate);
}

Tempo
TempoMap::tempo_at_frame (const framepos_t& frame) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return tempo_at_frame_locked (_metrics, frame);
}

Tempo
TempoMap::tempo_at_frame_locked (const Metrics& metrics, const framepos_t& frame) const
{
	TempoSection* prev_t = 0;

	TempoSection* t;

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if ((prev_t) && t->frame() > frame) {
				/* t is the section past frame */
				const double ret_bpm = prev_t->tempo_at_frame (frame, _frame_rate) * prev_t->note_type();
				const Tempo ret_tempo (ret_bpm, prev_t->note_type());
				return ret_tempo;
			}
			prev_t = t;
		}
	}

	const double ret = prev_t->beats_per_minute();
	const Tempo ret_tempo (ret, prev_t->note_type ());

	return ret_tempo;
}

/** returns the frame at which the supplied tempo occurs, or
 *  the frame of the last tempo section (search exhausted)
 *  only the position of the first occurence will be returned
 *  (extend me)
*/
framepos_t
TempoMap::frame_at_tempo (const Tempo& tempo) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return frame_at_tempo_locked (_metrics, tempo);
}


framepos_t
TempoMap::frame_at_tempo_locked (const Metrics& metrics, const Tempo& tempo) const
{
	TempoSection* prev_t = 0;
	const double tempo_ppm = tempo.beats_per_minute() / tempo.note_type();

	Metrics::const_iterator i;

	for (i = _metrics.begin(); i != _metrics.end(); ++i) {
		TempoSection* t;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);

			if (!t->active()) {
				continue;
			}

			const double t_ppm = t->beats_per_minute() / t->note_type();

			if (t_ppm == tempo_ppm) {
				return t->frame();
			}

			if (prev_t) {
				const double prev_t_ppm = prev_t->beats_per_minute() / prev_t->note_type();

				if ((t_ppm > tempo_ppm && prev_t_ppm < tempo_ppm) || (t_ppm < tempo_ppm && prev_t_ppm > tempo_ppm)) {
					return prev_t->frame_at_tempo (tempo_ppm, prev_t->pulse(), _frame_rate);
				}
			}
			prev_t = t;
		}
	}

	return prev_t->frame();
}

/** more precise than doing tempo_at_frame (frame_at_beat (b)),
 *  as there is no intermediate frame rounding.
 */
Tempo
TempoMap::tempo_at_beat (const double& beat) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	const MeterSection* prev_m = &meter_section_at_beat_locked (_metrics, beat);
	const TempoSection* prev_t = &tempo_section_at_beat_locked (_metrics, beat);
	const double note_type = prev_t->note_type();

	return Tempo (prev_t->tempo_at_pulse (((beat - prev_m->beat()) / prev_m->note_divisor()) + prev_m->pulse()) * note_type, note_type);
}

double
TempoMap::pulse_at_beat (const double& beat) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return pulse_at_beat_locked (_metrics, beat);
}

double
TempoMap::pulse_at_beat_locked (const Metrics& metrics, const double& beat) const
{
	const MeterSection* prev_m = &meter_section_at_beat_locked (metrics, beat);

	return prev_m->pulse() + ((beat - prev_m->beat()) / prev_m->note_divisor());
}

double
TempoMap::beat_at_pulse (const double& pulse) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return beat_at_pulse_locked (_metrics, pulse);
}

double
TempoMap::beat_at_pulse_locked (const Metrics& metrics, const double& pulse) const
{
	MeterSection* prev_m = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		MeterSection* m;
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m && m->pulse() > pulse) {
				if (((pulse - prev_m->pulse()) * prev_m->note_divisor()) + prev_m->beat() > m->beat()) {
					break;
				}
			}
			prev_m = m;
		}
	}

	double const ret = ((pulse - prev_m->pulse()) * prev_m->note_divisor()) + prev_m->beat();
	return ret;
}

double
TempoMap::pulse_at_frame (const framepos_t& frame) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return pulse_at_frame_locked (_metrics, frame);
}

/* tempo section based */
double
TempoMap::pulse_at_frame_locked (const Metrics& metrics, const framepos_t& frame) const
{
	/* HOLD (at least) THE READER LOCK */
	TempoSection* prev_t = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		TempoSection* t;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if (prev_t && t->frame() > frame) {
				/*the previous ts is the one containing the frame */
				const double ret = prev_t->pulse_at_frame (frame, _frame_rate);
				return ret;
			}
			prev_t = t;
		}
	}

	/* treated as constant for this ts */
	const double pulses_in_section = (frame - prev_t->frame()) / prev_t->frames_per_pulse (_frame_rate);

	return pulses_in_section + prev_t->pulse();
}

framepos_t
TempoMap::frame_at_pulse (const double& pulse) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return frame_at_pulse_locked (_metrics, pulse);
}

/* tempo section based */
framepos_t
TempoMap::frame_at_pulse_locked (const Metrics& metrics, const double& pulse) const
{
	/* HOLD THE READER LOCK */

	const TempoSection* prev_t = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		TempoSection* t;

		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if (prev_t && t->pulse() > pulse) {
				return prev_t->frame_at_pulse (pulse, _frame_rate);
			}

			prev_t = t;
		}
	}
	/* must be treated as constant, irrespective of _type */
	double const dtime = (pulse - prev_t->pulse()) * prev_t->frames_per_pulse (_frame_rate);

	return (framecnt_t) floor (dtime) + prev_t->frame();
}

double
TempoMap::beat_at_bbt (const Timecode::BBT_Time& bbt)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return beat_at_bbt_locked (_metrics, bbt);
}


double
TempoMap::beat_at_bbt_locked (const Metrics& metrics, const Timecode::BBT_Time& bbt) const
{
	/* CALLER HOLDS READ LOCK */

	MeterSection* prev_m = 0;

	/* because audio-locked meters have 'fake' integral beats,
	   there is no pulse offset here.
	*/
	MeterSection* m;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m) {
				const double bars_to_m = (m->beat() - prev_m->beat()) / prev_m->divisions_per_bar();
				if ((bars_to_m + (prev_m->bbt().bars - 1)) > (bbt.bars - 1)) {
					break;
				}
			}
			prev_m = m;
		}
	}

	const double remaining_bars = bbt.bars - prev_m->bbt().bars;
	const double remaining_bars_in_beats = remaining_bars * prev_m->divisions_per_bar();
	const double ret = remaining_bars_in_beats + prev_m->beat() + (bbt.beats - 1) + (bbt.ticks / BBT_Time::ticks_per_beat);

	return ret;
}

Timecode::BBT_Time
TempoMap::bbt_at_beat (const double& beats)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return bbt_at_beat_locked (_metrics, beats);
}

Timecode::BBT_Time
TempoMap::bbt_at_beat_locked (const Metrics& metrics, const double& b) const
{
	/* CALLER HOLDS READ LOCK */
	MeterSection* prev_m = 0;
	const double beats = max (0.0, b);

	MeterSection* m = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m) {
				if (m->beat() > beats) {
					/* this is the meter after the one our beat is on*/
					break;
				}
			}

			prev_m = m;
		}
	}

	const double beats_in_ms = beats - prev_m->beat();
	const uint32_t bars_in_ms = (uint32_t) floor (beats_in_ms / prev_m->divisions_per_bar());
	const uint32_t total_bars = bars_in_ms + (prev_m->bbt().bars - 1);
	const double remaining_beats = beats_in_ms - (bars_in_ms * prev_m->divisions_per_bar());
	const double remaining_ticks = (remaining_beats - floor (remaining_beats)) * BBT_Time::ticks_per_beat;

	BBT_Time ret;

	ret.ticks = (uint32_t) floor (remaining_ticks + 0.5);
	ret.beats = (uint32_t) floor (remaining_beats);
	ret.bars = total_bars;

	/* 0 0 0 to 1 1 0 - based mapping*/
	++ret.bars;
	++ret.beats;

	if (ret.ticks >= BBT_Time::ticks_per_beat) {
		++ret.beats;
		ret.ticks -= BBT_Time::ticks_per_beat;
	}

	if (ret.beats >= prev_m->divisions_per_bar() + 1) {
		++ret.bars;
		ret.beats = 1;
	}

	return ret;
}

double
TempoMap::pulse_at_bbt (const Timecode::BBT_Time& bbt)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return pulse_at_bbt_locked (_metrics, bbt);
}

double
TempoMap::pulse_at_bbt_rt (const Timecode::BBT_Time& bbt)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	if (!lm.locked()) {
		throw std::logic_error ("TempoMap::pulse_at_bbt_rt() could not lock tempo map");
	}

	return pulse_at_bbt_locked (_metrics, bbt);
}

double
TempoMap::pulse_at_bbt_locked (const Metrics& metrics, const Timecode::BBT_Time& bbt) const
{
	/* CALLER HOLDS READ LOCK */

	MeterSection* prev_m = 0;

	/* because audio-locked meters have 'fake' integral beats,
	   there is no pulse offset here.
	*/
	MeterSection* m;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m) {
				if (m->bbt().bars > bbt.bars) {
					break;
				}
			}
			prev_m = m;
		}
	}

	const double remaining_bars = bbt.bars - prev_m->bbt().bars;
	const double remaining_pulses = remaining_bars * prev_m->divisions_per_bar() / prev_m->note_divisor();
	const double ret = remaining_pulses + prev_m->pulse() + (((bbt.beats - 1) + (bbt.ticks / BBT_Time::ticks_per_beat)) / prev_m->note_divisor());

	return ret;
}

Timecode::BBT_Time
TempoMap::bbt_at_pulse (const double& pulse)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return bbt_at_pulse_locked (_metrics, pulse);
}

Timecode::BBT_Time
TempoMap::bbt_at_pulse_locked (const Metrics& metrics, const double& pulse) const
{
	MeterSection* prev_m = 0;

	MeterSection* m = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {

		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);

			if (prev_m) {
				double const pulses_to_m = m->pulse() - prev_m->pulse();
				if (prev_m->pulse() + pulses_to_m > pulse) {
					/* this is the meter after the one our beat is on*/
					break;
				}
			}

			prev_m = m;
		}
	}

	const double beats_in_ms = (pulse - prev_m->pulse()) * prev_m->note_divisor();
	const uint32_t bars_in_ms = (uint32_t) floor (beats_in_ms / prev_m->divisions_per_bar());
	const uint32_t total_bars = bars_in_ms + (prev_m->bbt().bars - 1);
	const double remaining_beats = beats_in_ms - (bars_in_ms * prev_m->divisions_per_bar());
	const double remaining_ticks = (remaining_beats - floor (remaining_beats)) * BBT_Time::ticks_per_beat;

	BBT_Time ret;

	ret.ticks = (uint32_t) floor (remaining_ticks + 0.5);
	ret.beats = (uint32_t) floor (remaining_beats);
	ret.bars = total_bars;

	/* 0 0 0 to 1 1 0 mapping*/
	++ret.bars;
	++ret.beats;

	if (ret.ticks >= BBT_Time::ticks_per_beat) {
		++ret.beats;
		ret.ticks -= BBT_Time::ticks_per_beat;
	}

	if (ret.beats >= prev_m->divisions_per_bar() + 1) {
		++ret.bars;
		ret.beats = 1;
	}

	return ret;
}

BBT_Time
TempoMap::bbt_at_frame (framepos_t frame)
{
	if (frame < 0) {
		BBT_Time bbt;
		bbt.bars = 1;
		bbt.beats = 1;
		bbt.ticks = 0;
		warning << string_compose (_("tempo map asked for BBT time at frame %1\n"), frame) << endmsg;
		return bbt;
	}
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return bbt_at_frame_locked (_metrics, frame);
}

BBT_Time
TempoMap::bbt_at_frame_rt (framepos_t frame)
{
	Glib::Threads::RWLock::ReaderLock lm (lock, Glib::Threads::TRY_LOCK);

	if (!lm.locked()) {
		throw std::logic_error ("TempoMap::bbt_at_frame_rt() could not lock tempo map");
	}

	return bbt_at_frame_locked (_metrics, frame);
}

Timecode::BBT_Time
TempoMap::bbt_at_frame_locked (const Metrics& metrics, const framepos_t& frame) const
{
	if (frame < 0) {
		BBT_Time bbt;
		bbt.bars = 1;
		bbt.beats = 1;
		bbt.ticks = 0;
		return bbt;
	}

	const TempoSection& ts = tempo_section_at_frame_locked (metrics, frame);
	MeterSection* prev_m = 0;
	MeterSection* next_m = 0;

	MeterSection* m;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m && m->frame() > frame) {
				next_m = m;
				break;
			}
			prev_m = m;
		}
	}

	double beat = prev_m->beat() + (ts.pulse_at_frame (frame, _frame_rate) - prev_m->pulse()) * prev_m->note_divisor();

	/* handle frame before first meter */
	if (frame < prev_m->frame()) {
		beat = 0.0;
	}
	/* audio locked meters fake their beat */
	if (next_m && next_m->beat() < beat) {
		beat = next_m->beat();
	}

	beat = max (0.0, beat);

	const double beats_in_ms = beat - prev_m->beat();
	const uint32_t bars_in_ms = (uint32_t) floor (beats_in_ms / prev_m->divisions_per_bar());
	const uint32_t total_bars = bars_in_ms + (prev_m->bbt().bars - 1);
	const double remaining_beats = beats_in_ms - (bars_in_ms * prev_m->divisions_per_bar());
	const double remaining_ticks = (remaining_beats - floor (remaining_beats)) * BBT_Time::ticks_per_beat;

	BBT_Time ret;

	ret.ticks = (uint32_t) floor (remaining_ticks + 0.5);
	ret.beats = (uint32_t) floor (remaining_beats);
	ret.bars = total_bars;

	/* 0 0 0 to 1 1 0 - based mapping*/
	++ret.bars;
	++ret.beats;

	if (ret.ticks >= BBT_Time::ticks_per_beat) {
		++ret.beats;
		ret.ticks -= BBT_Time::ticks_per_beat;
	}

	if (ret.beats >= prev_m->divisions_per_bar() + 1) {
		++ret.bars;
		ret.beats = 1;
	}

	return ret;
}

framepos_t
TempoMap::frame_at_bbt (const BBT_Time& bbt)
{
	if (bbt.bars < 1) {
		warning << string_compose (_("tempo map asked for frame time at bar < 1  (%1)\n"), bbt) << endmsg;
		return 0;
	}

	if (bbt.beats < 1) {
		throw std::logic_error ("beats are counted from one");
	}
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return frame_at_bbt_locked (_metrics, bbt);
}

/* meter & tempo section based */
framepos_t
TempoMap::frame_at_bbt_locked (const Metrics& metrics, const BBT_Time& bbt) const
{
	/* HOLD THE READER LOCK */

	const framepos_t ret = frame_at_beat_locked (metrics, beat_at_bbt_locked (metrics, bbt));
	return ret;
}

/**
 * Returns the distance from 0 in quarter pulses at the supplied frame.
 *
 * Plugin APIs don't count ticks in the same way PROGRAM_NAME does.
 * We use ticks per beat whereas the rest of the world uses ticks per quarter note.
 * This is more or less the VST's ppqPos (a scalar you use to obtain tick position
 * in whatever ppqn you're using).
 *
 * @param frame The distance in frames relative to session 0 whose quarter note distance you would like.
 * @return The quarter note (quarter pulse) distance from session 0 to the supplied frame. Ignores meter.
*/

double
TempoMap::quarter_note_at_frame (const framepos_t frame)
{
	Glib::Threads::RWLock::ReaderLock lm (lock, Glib::Threads::TRY_LOCK);

	const double ret = pulse_at_frame_locked (_metrics, frame) * 4.0;

	return ret;
}

double
TempoMap::quarter_note_at_frame_rt (const framepos_t frame)
{
	Glib::Threads::RWLock::ReaderLock lm (lock, Glib::Threads::TRY_LOCK);

	if (!lm.locked()) {
		throw std::logic_error ("TempoMap::quarter_note_at_frame_rt() could not lock tempo map");
	}

	const double ret = pulse_at_frame_locked (_metrics, frame) * 4.0;

	return ret;
}

framepos_t
TempoMap::frame_at_quarter_note (const double quarter_note)
{
	Glib::Threads::RWLock::ReaderLock lm (lock, Glib::Threads::TRY_LOCK);

	const framepos_t ret = frame_at_pulse_locked (_metrics, quarter_note / 4.0);

	return ret;
}

bool
TempoMap::check_solved (const Metrics& metrics) const
{
	TempoSection* prev_t = 0;
	MeterSection* prev_m = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		TempoSection* t;
		MeterSection* m;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if (prev_t) {
				/* check ordering */
				if ((t->frame() <= prev_t->frame()) || (t->pulse() <= prev_t->pulse())) {
					return false;
				}

				/* precision check ensures tempo and frames align.*/
				if (t->frame() != prev_t->frame_at_tempo (t->pulses_per_minute(), t->pulse(), _frame_rate)) {
					if (!t->locked_to_meter()) {
						return false;
					}
				}

				/* gradient limit - who knows what it should be?
				   things are also ok (if a little chaotic) without this
				*/
				if (fabs (prev_t->c_func()) > 1000.0) {
					//std::cout << "c : " << prev_t->c_func() << std::endl;
					return false;
				}
			}
			prev_t = t;
		}

		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m && m->position_lock_style() == AudioTime) {
				const TempoSection* t = &tempo_section_at_frame_locked (metrics, m->frame() - 1);
				const framepos_t nascent_m_frame = t->frame_at_pulse (m->pulse(), _frame_rate);
				/* Here we check that a preceding section of music doesn't overlap a subsequent one.
				   It is complicated by the fact that audio locked meters represent a discontinuity in the pulse
				   (they place an exact pulse at a particular time expressed only in frames).
				   This has the effect of shifting the calculated frame at the meter pulse (wrt the previous section of music)
				   away from its actual frame (which is now the frame location of the exact pulse).
				   This can result in the calculated frame (from the previous musical section)
				   differing from the exact frame by one sample.
				   Allow for that.
				*/
				if (t && (nascent_m_frame > m->frame() + 1 || nascent_m_frame < 0)) {
					return false;
				}
			}

			prev_m = m;
		}

	}

	return true;
}

bool
TempoMap::set_active_tempos (const Metrics& metrics, const framepos_t& frame)
{
	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		TempoSection* t;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->movable()) {
				t->set_active (true);
				continue;
			}
			if (t->movable() && t->active () && t->position_lock_style() == AudioTime && t->frame() < frame) {
				t->set_active (false);
				t->set_pulse (0.0);
			} else if (t->movable() && t->position_lock_style() == AudioTime && t->frame() > frame) {
				t->set_active (true);
			} else if (t->movable() && t->position_lock_style() == AudioTime && t->frame() == frame) {
				return false;
			}
		}
	}
	return true;
}

bool
TempoMap::solve_map_frame (Metrics& imaginary, TempoSection* section, const framepos_t& frame)
{
	TempoSection* prev_t = 0;
	TempoSection* section_prev = 0;
	framepos_t first_m_frame = 0;

	/* can't move a tempo before the first meter */
	for (Metrics::iterator i = imaginary.begin(); i != imaginary.end(); ++i) {
		MeterSection* m;
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (!m->movable()) {
				first_m_frame = m->frame();
				break;
			}
		}
	}
	if (section->movable() && frame <= first_m_frame) {
		return false;
	}

	section->set_active (true);
	section->set_frame (frame);

	for (Metrics::iterator i = imaginary.begin(); i != imaginary.end(); ++i) {
		TempoSection* t;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);

			if (!t->active()) {
				continue;
			}
			if (prev_t) {
				if (t == section) {
					section_prev = prev_t;
					if (t->locked_to_meter()) {
						prev_t = t;
					}
					continue;
				}
				if (t->position_lock_style() == MusicTime) {
					prev_t->set_c_func (prev_t->compute_c_func_pulse (t->pulses_per_minute(), t->pulse(), _frame_rate));
					t->set_frame (prev_t->frame_at_pulse (t->pulse(), _frame_rate));
				} else {
					prev_t->set_c_func (prev_t->compute_c_func_frame (t->pulses_per_minute(), t->frame(), _frame_rate));
					if (!t->locked_to_meter()) {
						t->set_pulse (prev_t->pulse_at_frame (t->frame(), _frame_rate));
					}
				}
			}
			prev_t = t;
		}
	}

	if (section_prev) {
		section_prev->set_c_func (section_prev->compute_c_func_frame (section->pulses_per_minute(), frame, _frame_rate));
		if (!section->locked_to_meter()) {
			section->set_pulse (section_prev->pulse_at_frame (frame, _frame_rate));
		}
	}

#if (0)
	recompute_tempi (imaginary);

	if (check_solved (imaginary)) {
		return true;
	} else {
		dunp (imaginary, std::cout);
	}
#endif

	MetricSectionFrameSorter fcmp;
	imaginary.sort (fcmp);

	recompute_tempi (imaginary);

	if (check_solved (imaginary)) {
		return true;
	}

	return false;
}

bool
TempoMap::solve_map_pulse (Metrics& imaginary, TempoSection* section, const double& pulse)
{
	TempoSection* prev_t = 0;
	TempoSection* section_prev = 0;

	section->set_pulse (pulse);

	for (Metrics::iterator i = imaginary.begin(); i != imaginary.end(); ++i) {
		TempoSection* t;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if (!t->movable()) {
				t->set_pulse (0.0);
				prev_t = t;
				continue;
			}
			if (prev_t) {
				if (t == section) {
					section_prev = prev_t;
					continue;
				}
				if (t->position_lock_style() == MusicTime) {
					prev_t->set_c_func (prev_t->compute_c_func_pulse (t->pulses_per_minute(), t->pulse(), _frame_rate));
					t->set_frame (prev_t->frame_at_pulse (t->pulse(), _frame_rate));
				} else {
					prev_t->set_c_func (prev_t->compute_c_func_frame (t->pulses_per_minute(), t->frame(), _frame_rate));
					if (!t->locked_to_meter()) {
						t->set_pulse (prev_t->pulse_at_frame (t->frame(), _frame_rate));
					}
				}
			}
			prev_t = t;
		}
	}

	if (section_prev) {
		section_prev->set_c_func (section_prev->compute_c_func_pulse (section->pulses_per_minute(), pulse, _frame_rate));
		section->set_frame (section_prev->frame_at_pulse (pulse, _frame_rate));
	}

#if (0)
	recompute_tempi (imaginary);

	if (check_solved (imaginary)) {
		return true;
	} else {
		dunp (imaginary, std::cout);
	}
#endif

	MetricSectionSorter cmp;
	imaginary.sort (cmp);

	recompute_tempi (imaginary);
	/* Reordering
	 * XX need a restriction here, but only for this case,
	 * as audio locked tempos don't interact in the same way.
	 *
	 * With music-locked tempos, the solution to cross-dragging can fly off the screen
	 * e.g.
	 * |50 bpm                        |250 bpm |60 bpm
	 *                drag 250 to the pulse after 60->
	 * a clue: dragging the second 60 <- past the 250 would cause no such problem.
	 */
	if (check_solved (imaginary)) {
		return true;
	}

	return false;
}

bool
TempoMap::solve_map_frame (Metrics& imaginary, MeterSection* section, const framepos_t& frame)
{
	/* disallow moving first meter past any subsequent one, and any movable meter before the first one */
	const MeterSection* other =  &meter_section_at_frame_locked (imaginary, frame);
	if ((!section->movable() && other->movable()) || (!other->movable() && section->movable() && other->frame() >= frame)) {
		return false;
	}

	if (!section->movable()) {
		/* lock the first tempo to our first meter */
		if (!set_active_tempos (imaginary, frame)) {
			return false;
		}
	}

	TempoSection* meter_locked_tempo = 0;

	for (Metrics::const_iterator ii = imaginary.begin(); ii != imaginary.end(); ++ii) {
		TempoSection* t;
		if ((*ii)->is_tempo()) {
			t = static_cast<TempoSection*> (*ii);
			if ((t->locked_to_meter() || !t->movable()) && t->frame() == section->frame()) {
				meter_locked_tempo = t;
				break;
			}
		}
	}

	if (!meter_locked_tempo) {
		return false;
	}

	MeterSection* prev_m = 0;
	Metrics future_map;
	TempoSection* tempo_copy = copy_metrics_and_point (imaginary, future_map, meter_locked_tempo);
	bool solved = false;

	for (Metrics::iterator i = imaginary.begin(); i != imaginary.end(); ++i) {
		MeterSection* m;
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (m == section){
				if (prev_m && section->movable()) {
					const double beats = (pulse_at_frame_locked (imaginary, frame) - prev_m->pulse()) * prev_m->note_divisor();
					if (beats + prev_m->beat() < section->beat()) {
						/* set the frame/pulse corresponding to its musical position,
						 * as an earlier time than this has been requested.
						*/
						const double new_pulse = ((section->beat() - prev_m->beat())
									  / prev_m->note_divisor()) + prev_m->pulse();

						const framepos_t smallest_frame = frame_at_pulse_locked (future_map, new_pulse);

						if ((solved = solve_map_frame (future_map, tempo_copy, smallest_frame))) {
							meter_locked_tempo->set_pulse (new_pulse);
							solve_map_frame (imaginary, meter_locked_tempo, smallest_frame);
							section->set_frame (smallest_frame);
							section->set_pulse (new_pulse);
						} else {
							solved = false;
						}

						Metrics::const_iterator d = future_map.begin();
						while (d != future_map.end()) {
							delete (*d);
							++d;
						}

						if (!solved) {
							return false;
						}
					} else {
						/* all is ok. set section's locked tempo if allowed.
						   possibly disallowed if there is an adjacent audio-locked tempo.
						   XX this check could possibly go. its never actually happened here.
						*/
						MeterSection* meter_copy = const_cast<MeterSection*> (&meter_section_at_frame_locked (future_map, section->frame()));
						meter_copy->set_frame (frame);

						if ((solved = solve_map_frame (future_map, tempo_copy, frame))) {
							section->set_frame (frame);
							meter_locked_tempo->set_pulse (((section->beat() - prev_m->beat())
												/ prev_m->note_divisor()) + prev_m->pulse());
							solve_map_frame (imaginary, meter_locked_tempo, frame);
						} else {
							solved = false;
						}

						Metrics::const_iterator d = future_map.begin();
						while (d != future_map.end()) {
							delete (*d);
							++d;
						}

						if (!solved) {
							return false;
						}
					}
				} else {
					/* not movable (first meter atm) */

					tempo_copy->set_frame (frame);
					tempo_copy->set_pulse (0.0);

					if ((solved = solve_map_frame (future_map, tempo_copy, frame))) {
						section->set_frame (frame);
						meter_locked_tempo->set_frame (frame);
						meter_locked_tempo->set_pulse (0.0);
						solve_map_frame (imaginary, meter_locked_tempo, frame);
					} else {
						solved = false;
					}

					Metrics::const_iterator d = future_map.begin();
					while (d != future_map.end()) {
						delete (*d);
						++d;
					}

					if (!solved) {
						return false;
					}

					pair<double, BBT_Time> b_bbt = make_pair (0.0, BBT_Time (1, 1, 0));
					section->set_beat (b_bbt);
					section->set_pulse (0.0);

				}
				break;
			}

			prev_m = m;
		}
	}

	MetricSectionFrameSorter fcmp;
	imaginary.sort (fcmp);

	recompute_meters (imaginary);

	return true;
}

bool
TempoMap::solve_map_bbt (Metrics& imaginary, MeterSection* section, const BBT_Time& when)
{
	/* disallow setting section to an existing meter's bbt */
	for (Metrics::iterator i = imaginary.begin(); i != imaginary.end(); ++i) {
		MeterSection* m;
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (m != section && m->bbt().bars == when.bars) {
				return false;
			}
		}
	}

	MeterSection* prev_m = 0;
	MeterSection* section_prev = 0;

	for (Metrics::iterator i = imaginary.begin(); i != imaginary.end(); ++i) {
		MeterSection* m;
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			pair<double, BBT_Time> b_bbt;
			double new_pulse = 0.0;

			if (prev_m && m->bbt().bars > when.bars && !section_prev){
				section_prev = prev_m;
				const double beats = (when.bars - section_prev->bbt().bars) * section_prev->divisions_per_bar();
				const double pulse = (beats / section_prev->note_divisor()) + section_prev->pulse();
				pair<double, BBT_Time> b_bbt = make_pair (beats + section_prev->beat(), when);

				section->set_beat (b_bbt);
				section->set_pulse (pulse);
				section->set_frame (frame_at_pulse_locked (imaginary, pulse));
				prev_m = section;
				continue;
			}

			if (m->position_lock_style() == AudioTime) {
				TempoSection* meter_locked_tempo = 0;

				for (Metrics::const_iterator ii = imaginary.begin(); ii != imaginary.end(); ++ii) {
					TempoSection* t;
					if ((*ii)->is_tempo()) {
						t = static_cast<TempoSection*> (*ii);
						if ((t->locked_to_meter() || !t->movable()) && t->frame() == m->frame()) {
							meter_locked_tempo = t;
							break;
						}
					}
				}

				if (!meter_locked_tempo) {
					return false;
				}

				if (prev_m) {
					const double beats = ((m->bbt().bars - prev_m->bbt().bars) * prev_m->divisions_per_bar());

					if (beats + prev_m->beat() != m->beat()) {
						/* tempo/ meter change caused a change in beat (bar). */
						b_bbt = make_pair (beats + prev_m->beat()
								   , BBT_Time ((beats / prev_m->divisions_per_bar()) + prev_m->bbt().bars, 1, 0));
						new_pulse = prev_m->pulse() + (beats / prev_m->note_divisor());
					} else if (m->movable()) {
						b_bbt = make_pair (m->beat(), m->bbt());
						new_pulse = prev_m->pulse() + (beats / prev_m->note_divisor());
					}
				} else {
					b_bbt = make_pair (0.0, BBT_Time (1, 1, 0));
				}

				meter_locked_tempo->set_pulse (new_pulse);
				m->set_beat (b_bbt);
				m->set_pulse (new_pulse);

			} else {
				/* MusicTime */
				const double beats = ((m->bbt().bars - prev_m->bbt().bars) * prev_m->divisions_per_bar());
				if (beats + prev_m->beat() != m->beat()) {
					/* tempo/ meter change caused a change in beat (bar). */
					b_bbt = make_pair (beats + prev_m->beat()
							   , BBT_Time ((beats / prev_m->divisions_per_bar()) + prev_m->bbt().bars, 1, 0));
				} else {
					b_bbt = make_pair (beats + prev_m->beat()
							   , m->bbt());
				}
				new_pulse = (beats / prev_m->note_divisor()) + prev_m->pulse();
				m->set_beat (b_bbt);
				m->set_pulse (new_pulse);
				m->set_frame (frame_at_pulse_locked (imaginary, new_pulse));
			}

			prev_m = m;
		}
	}

	if (!section_prev) {

		const double beats = (when.bars - prev_m->bbt().bars) * prev_m->divisions_per_bar();
		const double pulse = (beats / prev_m->note_divisor()) + prev_m->pulse();
		pair<double, BBT_Time> b_bbt = make_pair (beats + prev_m->beat(), when);

		section->set_beat (b_bbt);
		section->set_pulse (pulse);
		section->set_frame (frame_at_pulse_locked (imaginary, pulse));
	}

	MetricSectionSorter cmp;
	imaginary.sort (cmp);

	recompute_meters (imaginary);

	return true;
}

/** places a copy of _metrics into copy and returns a pointer
 *  to section's equivalent in copy.
 */
TempoSection*
TempoMap::copy_metrics_and_point (const Metrics& metrics, Metrics& copy, TempoSection* section)
{
	TempoSection* ret = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		TempoSection* t;
		MeterSection* m;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (t == section) {
				ret = new TempoSection (*t);
				copy.push_back (ret);
				continue;
			}

			TempoSection* cp = new TempoSection (*t);
			copy.push_back (cp);
		}
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection *> (*i);
			MeterSection* cp = new MeterSection (*m);
			copy.push_back (cp);
		}
	}

	return ret;
}

MeterSection*
TempoMap::copy_metrics_and_point (const Metrics& metrics, Metrics& copy, MeterSection* section)
{
	MeterSection* ret = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		TempoSection* t;
		MeterSection* m;
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			TempoSection* cp = new TempoSection (*t);
			copy.push_back (cp);
		}

		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection *> (*i);
			if (m == section) {
				ret = new MeterSection (*m);
				copy.push_back (ret);
				continue;
			}
			MeterSection* cp = new MeterSection (*m);
			copy.push_back (cp);
		}
	}

	return ret;
}

/** answers the question "is this a valid beat position for this tempo section?".
 *  it returns true if the tempo section can be moved to the requested bbt position,
 *  leaving the tempo map in a solved state.
 * @param section the tempo section to be moved
 * @param bbt the requested new position for the tempo section
 * @return true if the tempo section can be moved to the position, otherwise false.
 */
bool
TempoMap::can_solve_bbt (TempoSection* ts, const BBT_Time& bbt)
{
	Metrics copy;
	TempoSection* tempo_copy = 0;

	{
		Glib::Threads::RWLock::ReaderLock lm (lock);
		tempo_copy = copy_metrics_and_point (_metrics, copy, ts);
		if (!tempo_copy) {
			return false;
		}
	}

	const bool ret = solve_map_pulse (copy, tempo_copy, pulse_at_bbt_locked (copy, bbt));

	Metrics::const_iterator d = copy.begin();
	while (d != copy.end()) {
		delete (*d);
		++d;
	}

	return ret;
}

/**
* This is for a gui that needs to know the pulse or frame of a tempo section if it were to be moved to some bbt time,
* taking any possible reordering as a consequence of this into account.
* @param section - the section to be altered
* @param bbt - the bbt where the altered tempo will fall
* @return returns - the position in pulses and frames (as a pair) where the new tempo section will lie.
*/
pair<double, framepos_t>
TempoMap::predict_tempo_position (TempoSection* section, const BBT_Time& bbt)
{
	Metrics future_map;
	pair<double, framepos_t> ret = make_pair (0.0, 0);

	Glib::Threads::RWLock::ReaderLock lm (lock);

	TempoSection* tempo_copy = copy_metrics_and_point (_metrics, future_map, section);

	const double beat = beat_at_bbt_locked (future_map, bbt);

	if (solve_map_pulse (future_map, tempo_copy, pulse_at_beat_locked (future_map, beat))) {
		ret.first = tempo_copy->pulse();
		ret.second = tempo_copy->frame();
	} else {
		ret.first = section->pulse();
		ret.second = section->frame();
	}

	Metrics::const_iterator d = future_map.begin();
	while (d != future_map.end()) {
		delete (*d);
		++d;
	}
	return ret;
}

void
TempoMap::gui_move_tempo (TempoSection* ts, const framepos_t& frame, const int& sub_num)
{
	Metrics future_map;

	if (ts->position_lock_style() == MusicTime) {
		{
			/* if we're snapping to a musical grid, set the pulse exactly instead of via the supplied frame. */
			Glib::Threads::RWLock::WriterLock lm (lock);
			TempoSection* tempo_copy = copy_metrics_and_point (_metrics, future_map, ts);

			tempo_copy->set_position_lock_style (AudioTime);

			if (solve_map_frame (future_map, tempo_copy, frame)) {
				const double beat = exact_beat_at_frame_locked (future_map, frame, sub_num);
				const double pulse = pulse_at_beat_locked (future_map, beat);

				if (solve_map_pulse (future_map, tempo_copy, pulse)) {
					solve_map_pulse (_metrics, ts, pulse);
					recompute_meters (_metrics);
				}
			}
		}

	} else {

		{
			Glib::Threads::RWLock::WriterLock lm (lock);
			TempoSection* tempo_copy = copy_metrics_and_point (_metrics, future_map, ts);

			if (solve_map_frame (future_map, tempo_copy, frame)) {
				if (sub_num != 0) {
					/* We're moving the object that defines the grid while snapping to it...
					 * Placing the ts at the beat corresponding to the requested frame may shift the
					 * grid in such a way that the mouse is left hovering over a completerly different division,
					 * causing jittering when the mouse next moves (esp. large tempo deltas).
					 * To avoid this, place the ts at the requested frame in a dummy map
					 * then find the closest beat subdivision to that frame in the dummy.
					 * This alters the snap behaviour slightly in that we snap to beat divisions
					 * in the future map rather than the existing one.
					 */
					const double beat = exact_beat_at_frame_locked (future_map, frame, sub_num);
					const double pulse = pulse_at_beat_locked (future_map, beat);

					if (solve_map_pulse (future_map, tempo_copy, pulse)) {
						/* snapping to a grid. force MusicTime temporarily. */
						ts->set_position_lock_style (MusicTime);
						solve_map_pulse (_metrics, ts, pulse);
						ts->set_position_lock_style (AudioTime);

						recompute_meters (_metrics);
					}
				} else {
					solve_map_frame (_metrics, ts, frame);
					recompute_meters (_metrics);
				}
			}
		}
	}

	Metrics::const_iterator d = future_map.begin();
	while (d != future_map.end()) {
		delete (*d);
		++d;
	}

	MetricPositionChanged (); // Emit Signal
}

void
TempoMap::gui_move_meter (MeterSection* ms, const framepos_t& frame)
{
	Metrics future_map;

	if (ms->position_lock_style() == AudioTime) {

		{
			Glib::Threads::RWLock::WriterLock lm (lock);
			MeterSection* copy = copy_metrics_and_point (_metrics, future_map, ms);

			if (solve_map_frame (future_map, copy, frame)) {
				solve_map_frame (_metrics, ms, frame);
				recompute_tempi (_metrics);
			}
		}
	} else {
		{
			Glib::Threads::RWLock::WriterLock lm (lock);
			MeterSection* copy = copy_metrics_and_point (_metrics, future_map, ms);

			const double beat = beat_at_frame_locked (_metrics, frame);
			const Timecode::BBT_Time bbt = bbt_at_beat_locked (_metrics, beat);

			if (solve_map_bbt (future_map, copy, bbt)) {
				solve_map_bbt (_metrics, ms, bbt);
				recompute_tempi (_metrics);
			}
		}
	}

	Metrics::const_iterator d = future_map.begin();
	while (d != future_map.end()) {
		delete (*d);
		++d;
	}

	MetricPositionChanged (); // Emit Signal
}

bool
TempoMap::gui_change_tempo (TempoSection* ts, const Tempo& bpm)
{
	Metrics future_map;
	bool can_solve = false;
	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		TempoSection* tempo_copy = copy_metrics_and_point (_metrics, future_map, ts);
		tempo_copy->set_beats_per_minute (bpm.beats_per_minute());
		recompute_tempi (future_map);

		if (check_solved (future_map)) {
			ts->set_beats_per_minute (bpm.beats_per_minute());
			recompute_map (_metrics);
			can_solve = true;
		}
	}

	Metrics::const_iterator d = future_map.begin();
	while (d != future_map.end()) {
		delete (*d);
		++d;
	}
	if (can_solve) {
		MetricPositionChanged (); // Emit Signal
	}
	return can_solve;
}

void
TempoMap::gui_dilate_tempo (TempoSection* ts, const framepos_t& frame, const framepos_t& end_frame, const double& pulse)
{
	/*
	  Ts (future prev_t)   Tnext
	  |                    |
	  |     [drag^]        |
	  |----------|----------
	        e_f  pulse(frame)
	*/

	Metrics future_map;

	{
		Glib::Threads::RWLock::WriterLock lm (lock);

		if (!ts) {
			return;
		}

		TempoSection* prev_t = copy_metrics_and_point (_metrics, future_map, ts);
		TempoSection* prev_to_prev_t = 0;
		const frameoffset_t fr_off = end_frame - frame;

		if (prev_t && prev_t->pulse() > 0.0) {
			prev_to_prev_t = const_cast<TempoSection*>(&tempo_section_at_frame_locked (future_map, prev_t->frame() - 1));
		}

		TempoSection* next_t = 0;
		for (Metrics::iterator i = future_map.begin(); i != future_map.end(); ++i) {
			TempoSection* t = 0;
			if ((*i)->is_tempo()) {
				t = static_cast<TempoSection*> (*i);
				if (t->frame() > ts->frame()) {
					next_t = t;
					break;
				}
			}
		}
		/* minimum allowed measurement distance in frames */
		const framepos_t min_dframe = 2;

		/* the change in frames is the result of changing the slope of at most 2 previous tempo sections.
		   constant to constant is straightforward, as the tempo prev to prev_t has constant slope.
		*/
		double contribution = 0.0;

		if (next_t && prev_to_prev_t && prev_to_prev_t->type() == TempoSection::Ramp) {
			contribution = (prev_t->frame() - prev_to_prev_t->frame()) / (double) (next_t->frame() - prev_to_prev_t->frame());
		}

		const frameoffset_t prev_t_frame_contribution = fr_off - (contribution * (double) fr_off);

		const double start_pulse = prev_t->pulse_at_frame (frame, _frame_rate);
		const double end_pulse = prev_t->pulse_at_frame (end_frame, _frame_rate);

		double new_bpm;

		if (prev_t->type() == TempoSection::Constant || prev_t->c_func() == 0.0) {

			if (prev_t->position_lock_style() == MusicTime) {
				if (prev_to_prev_t && prev_to_prev_t->type() == TempoSection::Ramp) {
					if (frame > prev_to_prev_t->frame() + min_dframe && (frame + prev_t_frame_contribution) > prev_to_prev_t->frame() + min_dframe) {

						new_bpm = prev_t->beats_per_minute() * ((frame - prev_to_prev_t->frame())
											/ (double) ((frame + prev_t_frame_contribution) - prev_to_prev_t->frame()));
					} else {
						new_bpm = prev_t->beats_per_minute();
					}
				} else {
					/* prev to prev is irrelevant */

					if (start_pulse > prev_t->pulse() && end_pulse > prev_t->pulse()) {
						new_bpm = prev_t->beats_per_minute() * ((start_pulse - prev_t->pulse()) / (end_pulse - prev_t->pulse()));
					} else {
						new_bpm = prev_t->beats_per_minute();
					}
				}
			} else {
				/* AudioTime */
				if (prev_to_prev_t && prev_to_prev_t->type() == TempoSection::Ramp) {
					if (frame > prev_to_prev_t->frame() + min_dframe && end_frame > prev_to_prev_t->frame() + min_dframe) {

						new_bpm = prev_t->beats_per_minute() * ((frame - prev_to_prev_t->frame())
											/ (double) ((end_frame) - prev_to_prev_t->frame()));
					} else {
						new_bpm = prev_t->beats_per_minute();
					}
				} else {
					/* prev_to_prev_t is irrelevant */

					if (frame > prev_t->frame() + min_dframe && end_frame > prev_t->frame() + min_dframe) {
						new_bpm = prev_t->beats_per_minute() * ((frame - prev_t->frame()) / (double) (end_frame - prev_t->frame()));
					} else {
						new_bpm = prev_t->beats_per_minute();
					}
				}
			}
		} else {

			double frame_ratio = 1.0;
			double pulse_ratio = 1.0;
			const framepos_t pulse_pos = prev_t->frame_at_pulse (pulse, _frame_rate);

			if (prev_to_prev_t) {
				if (pulse_pos > prev_to_prev_t->frame() + min_dframe && (pulse_pos - fr_off) > prev_to_prev_t->frame() + min_dframe) {
					frame_ratio = (((pulse_pos - fr_off) - prev_to_prev_t->frame()) / (double) ((pulse_pos) - prev_to_prev_t->frame()));
				}
				if (end_pulse > prev_to_prev_t->pulse() && start_pulse > prev_to_prev_t->pulse()) {
					pulse_ratio = ((start_pulse - prev_to_prev_t->pulse()) / (end_pulse - prev_to_prev_t->pulse()));
				}
			} else {
				if (pulse_pos > prev_t->frame() + min_dframe && (pulse_pos - fr_off) > prev_t->frame() + min_dframe) {
					frame_ratio = (((pulse_pos - fr_off) - prev_t->frame()) / (double) ((pulse_pos) - prev_t->frame()));
				}
				pulse_ratio = (start_pulse / end_pulse);
			}
			new_bpm = prev_t->beats_per_minute() * (pulse_ratio * frame_ratio);
		}

		/* don't clamp and proceed here.
		   testing has revealed that this can go negative,
		   which is an entirely different thing to just being too low.
		*/
		if (new_bpm < 0.5) {
			return;
		}
		new_bpm = min (new_bpm, (double) 1000.0);
		prev_t->set_beats_per_minute (new_bpm);
		recompute_tempi (future_map);
		recompute_meters (future_map);

		if (check_solved (future_map)) {
			ts->set_beats_per_minute (new_bpm);
			recompute_tempi (_metrics);
			recompute_meters (_metrics);
		}
	}

	Metrics::const_iterator d = future_map.begin();
	while (d != future_map.end()) {
		delete (*d);
		++d;
	}

	MetricPositionChanged (); // Emit Signal
}

/** Returns the exact beat subdivision closest to the supplied frame, possibly returning a negative value.
 * @param frame  The session frame position.
 * @param sub_num The requested beat subdivision to use when rounding the frame position.
 * @return The beat position of the supplied frame.
 * If the supplied frame lies before the first meter, the return will be negative.
 * The returned beat is obtained using the first meter and the continuation of the tempo curve (backwards).
 *
 * This function uses both tempo and meter.
 */
double
TempoMap::exact_beat_at_frame (const framepos_t& frame, const int32_t sub_num)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return exact_beat_at_frame_locked (_metrics, frame, sub_num);
}

double
TempoMap::exact_beat_at_frame_locked (const Metrics& metrics, const framepos_t& frame, const int32_t sub_num)
{
	double beat = beat_at_frame_locked (metrics, frame);

	if (sub_num > 1) {
		beat = floor (beat) + (floor (((beat - floor (beat)) * (double) sub_num) + 0.5) / sub_num);
	} else if (sub_num == 1) {
		/* snap to beat */
		beat = floor (beat + 0.5);
	} else if (sub_num == -1) {
		/* snap to  bar */
		Timecode::BBT_Time bbt = bbt_at_beat_locked (metrics, beat);
		bbt.beats = 1;
		bbt.ticks = 0;

		const double prev_b = beat_at_bbt_locked (_metrics, bbt);
		++bbt.bars;
		const double next_b = beat_at_bbt_locked (_metrics, bbt);

		if ((beat - prev_b) > (next_b - prev_b) / 2.0) {
			beat = next_b;
		} else {
			beat = prev_b;
		}
	}

	return beat;
}

framecnt_t
TempoMap::bbt_duration_at (framepos_t pos, const BBT_Time& bbt, int dir)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	const double tick_at_time = max (0.0, beat_at_frame_locked (_metrics, pos)) * BBT_Time::ticks_per_beat;
	const double bbt_ticks = bbt.ticks + (bbt.beats * BBT_Time::ticks_per_beat);
	const double total_beats = (tick_at_time + bbt_ticks) / BBT_Time::ticks_per_beat;

	return frame_at_beat_locked (_metrics, total_beats);
}

framepos_t
TempoMap::round_to_bar (framepos_t fr, RoundMode dir)
{
	return round_to_type (fr, dir, Bar);
}

framepos_t
TempoMap::round_to_beat (framepos_t fr, RoundMode dir)
{
	return round_to_type (fr, dir, Beat);
}

framepos_t
TempoMap::round_to_beat_subdivision (framepos_t fr, int sub_num, RoundMode dir)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	uint32_t ticks = (uint32_t) floor (max (0.0, beat_at_frame_locked (_metrics, fr)) * BBT_Time::ticks_per_beat);
	uint32_t beats = (uint32_t) floor (ticks / BBT_Time::ticks_per_beat);
	uint32_t ticks_one_subdivisions_worth = (uint32_t) BBT_Time::ticks_per_beat / sub_num;

	ticks -= beats * BBT_Time::ticks_per_beat;

	if (dir > 0) {
		/* round to next (or same iff dir == RoundUpMaybe) */

		uint32_t mod = ticks % ticks_one_subdivisions_worth;

		if (mod == 0 && dir == RoundUpMaybe) {
			/* right on the subdivision, which is fine, so do nothing */

		} else if (mod == 0) {
			/* right on the subdivision, so the difference is just the subdivision ticks */
			ticks += ticks_one_subdivisions_worth;

		} else {
			/* not on subdivision, compute distance to next subdivision */

			ticks += ticks_one_subdivisions_worth - mod;
		}

		if (ticks >= BBT_Time::ticks_per_beat) {
			ticks -= BBT_Time::ticks_per_beat;
		}
	} else if (dir < 0) {

		/* round to previous (or same iff dir == RoundDownMaybe) */

		uint32_t difference = ticks % ticks_one_subdivisions_worth;

		if (difference == 0 && dir == RoundDownAlways) {
			/* right on the subdivision, but force-rounding down,
			   so the difference is just the subdivision ticks */
			difference = ticks_one_subdivisions_worth;
		}

		if (ticks < difference) {
			ticks = BBT_Time::ticks_per_beat - ticks;
		} else {
			ticks -= difference;
		}

	} else {
		/* round to nearest */
		double rem;

		/* compute the distance to the previous and next subdivision */

		if ((rem = fmod ((double) ticks, (double) ticks_one_subdivisions_worth)) > ticks_one_subdivisions_worth/2.0) {

			/* closer to the next subdivision, so shift forward */

			ticks = lrint (ticks + (ticks_one_subdivisions_worth - rem));

			DEBUG_TRACE (DEBUG::SnapBBT, string_compose ("moved forward to %1\n", ticks));

			if (ticks > BBT_Time::ticks_per_beat) {
				++beats;
				ticks -= BBT_Time::ticks_per_beat;
				DEBUG_TRACE (DEBUG::SnapBBT, string_compose ("fold beat to %1\n", beats));
			}

		} else if (rem > 0) {

			/* closer to previous subdivision, so shift backward */

			if (rem > ticks) {
				if (beats == 0) {
					/* can't go backwards past zero, so ... */
					return 0;
				}
				/* step back to previous beat */
				--beats;
				ticks = lrint (BBT_Time::ticks_per_beat - rem);
				DEBUG_TRACE (DEBUG::SnapBBT, string_compose ("step back beat to %1\n", beats));
			} else {
				ticks = lrint (ticks - rem);
				DEBUG_TRACE (DEBUG::SnapBBT, string_compose ("moved backward to %1\n", ticks));
			}
		} else {
			/* on the subdivision, do nothing */
		}
	}

	const framepos_t ret_frame = frame_at_beat_locked (_metrics, beats + (ticks / BBT_Time::ticks_per_beat));

	return ret_frame;
}

framepos_t
TempoMap::round_to_type (framepos_t frame, RoundMode dir, BBTPointType type)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	const double beat_at_framepos = max (0.0, beat_at_frame_locked (_metrics, frame));
	BBT_Time bbt (bbt_at_beat_locked (_metrics, beat_at_framepos));

	switch (type) {
	case Bar:
		if (dir < 0) {
			/* find bar previous to 'frame' */
			bbt.beats = 1;
			bbt.ticks = 0;
			return frame_at_bbt_locked (_metrics, bbt);

		} else if (dir > 0) {
			/* find bar following 'frame' */
			++bbt.bars;
			bbt.beats = 1;
			bbt.ticks = 0;
			return frame_at_bbt_locked (_metrics, bbt);
		} else {
			/* true rounding: find nearest bar */
			framepos_t raw_ft = frame_at_bbt_locked (_metrics, bbt);
			bbt.beats = 1;
			bbt.ticks = 0;
			framepos_t prev_ft = frame_at_bbt_locked (_metrics, bbt);
			++bbt.bars;
			framepos_t next_ft = frame_at_bbt_locked (_metrics, bbt);

			if ((raw_ft - prev_ft) > (next_ft - prev_ft) / 2) { 
				return next_ft;
			} else {
				return prev_ft;
			}
		}

		break;

	case Beat:
		if (dir < 0) {
			return frame_at_beat_locked (_metrics, floor (beat_at_framepos));
		} else if (dir > 0) {
			return frame_at_beat_locked (_metrics, ceil (beat_at_framepos));
		} else {
			return frame_at_beat_locked (_metrics, floor (beat_at_framepos + 0.5));
		}
		break;
	}

	return 0;
}

void
TempoMap::get_grid (vector<TempoMap::BBTPoint>& points,
		    framepos_t lower, framepos_t upper)
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	int32_t cnt = ceil (beat_at_frame_locked (_metrics, lower));
	framecnt_t pos = 0;
	/* although the map handles negative beats, bbt doesn't. */
	if (cnt < 0.0) {
		cnt = 0.0;
	}

	if (frame_at_beat_locked (_metrics, cnt) >= upper) {
		return;
	}

	while (pos < upper) {
		pos = frame_at_beat_locked (_metrics, cnt);
		const TempoSection tempo = tempo_section_at_frame_locked (_metrics, pos);
		const MeterSection meter = meter_section_at_frame_locked (_metrics, pos);
		const BBT_Time bbt = bbt_at_beat_locked (_metrics, cnt);
		points.push_back (BBTPoint (meter, tempo_at_frame_locked (_metrics, pos), pos, bbt.bars, bbt.beats, tempo.c_func()));
		++cnt;
	}
}

const TempoSection&
TempoMap::tempo_section_at_frame (framepos_t frame) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return tempo_section_at_frame_locked (_metrics, frame);
}

const TempoSection&
TempoMap::tempo_section_at_frame_locked (const Metrics& metrics, framepos_t frame) const
{
	TempoSection* prev = 0;

	TempoSection* t;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {

		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if (prev && t->frame() > frame) {
				break;
			}

			prev = t;
		}
	}

	if (prev == 0) {
		fatal << endmsg;
		abort(); /*NOTREACHED*/
	}

	return *prev;
}

const TempoSection&
TempoMap::tempo_section_at_beat_locked (const Metrics& metrics, const double& beat) const
{
	TempoSection* prev_t = 0;
	const MeterSection* prev_m = &meter_section_at_beat_locked (metrics, beat);

	TempoSection* t;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (prev_t && ((t->pulse() - prev_m->pulse()) * prev_m->note_divisor()) + prev_m->beat() > beat) {
				break;
			}
			prev_t = t;
		}

	}
	return *prev_t;
}

/* don't use this to calculate length (the tempo is only correct for this frame).
   do that stuff based on the beat_at_frame and frame_at_beat api
*/
double
TempoMap::frames_per_beat_at (const framepos_t& frame, const framecnt_t& sr) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	const TempoSection* ts_at = 0;
	const TempoSection* ts_after = 0;
	Metrics::const_iterator i;
	TempoSection* t;

	for (i = _metrics.begin(); i != _metrics.end(); ++i) {

		if ((*i)->is_tempo()) {
			t = static_cast<TempoSection*> (*i);
			if (!t->active()) {
				continue;
			}
			if (ts_at && (*i)->frame() > frame) {
				ts_after = t;
				break;
			}
			ts_at = t;
		}
	}

	if (ts_after) {
		return  (60.0 * _frame_rate) / (ts_at->tempo_at_frame (frame, _frame_rate) * ts_at->note_type());
	}
	/* must be treated as constant tempo */
	return ts_at->frames_per_beat (_frame_rate);
}

const MeterSection&
TempoMap::meter_section_at_frame_locked (const Metrics& metrics, framepos_t frame) const
{
	Metrics::const_iterator i;
	MeterSection* prev = 0;

	MeterSection* m;

	for (i = metrics.begin(); i != metrics.end(); ++i) {

		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);

			if (prev && (*i)->frame() > frame) {
				break;
			}

			prev = m;
		}
	}

	if (prev == 0) {
		fatal << endmsg;
		abort(); /*NOTREACHED*/
	}

	return *prev;
}


const MeterSection&
TempoMap::meter_section_at_frame (framepos_t frame) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return meter_section_at_frame_locked (_metrics, frame);
}

const MeterSection&
TempoMap::meter_section_at_beat_locked (const Metrics& metrics, const double& beat) const
{
	MeterSection* prev_m = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {
		MeterSection* m;
		if (!(*i)->is_tempo()) {
			m = static_cast<MeterSection*> (*i);
			if (prev_m && m->beat() > beat) {
				break;
			}
			prev_m = m;
		}

	}
	return *prev_m;
}

const MeterSection&
TempoMap::meter_section_at_beat (double beat) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	return meter_section_at_beat_locked (_metrics, beat);
}

const Meter&
TempoMap::meter_at_frame (framepos_t frame) const
{
	TempoMetric m (metric_at (frame));
	return m.meter();
}

void
TempoMap::fix_legacy_session ()
{
	MeterSection* prev_m = 0;
	TempoSection* prev_t = 0;

	for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		MeterSection* m;
		TempoSection* t;

		if ((m = dynamic_cast<MeterSection*>(*i)) != 0) {
			if (!m->movable()) {
				pair<double, BBT_Time> bbt = make_pair (0.0, BBT_Time (1, 1, 0));
				m->set_beat (bbt);
				m->set_pulse (0.0);
				m->set_frame (0);
				m->set_position_lock_style (AudioTime);
				prev_m = m;
				continue;
			}
			if (prev_m) {
				pair<double, BBT_Time> start = make_pair (((m->bbt().bars - 1) * prev_m->note_divisor())
									  + (m->bbt().beats - 1)
									  + (m->bbt().ticks / BBT_Time::ticks_per_beat)
									  , m->bbt());
				m->set_beat (start);
				const double start_beat = ((m->bbt().bars - 1) * prev_m->note_divisor())
					+ (m->bbt().beats - 1)
					+ (m->bbt().ticks / BBT_Time::ticks_per_beat);
				m->set_pulse (start_beat / prev_m->note_divisor());
			}
			prev_m = m;
		} else if ((t = dynamic_cast<TempoSection*>(*i)) != 0) {

			if (!t->active()) {
				continue;
			}

			if (!t->movable()) {
				t->set_pulse (0.0);
				t->set_frame (0);
				t->set_position_lock_style (AudioTime);
				prev_t = t;
				continue;
			}

			if (prev_t) {
				const double beat = ((t->legacy_bbt().bars - 1) * ((prev_m) ? prev_m->note_divisor() : 4.0))
					+ (t->legacy_bbt().beats - 1)
					+ (t->legacy_bbt().ticks / BBT_Time::ticks_per_beat);
				if (prev_m) {
					t->set_pulse (beat / prev_m->note_divisor());
				} else {
					/* really shouldn't happen but.. */
					t->set_pulse (beat / 4.0);
				}
			}
			prev_t = t;
		}
	}
}

XMLNode&
TempoMap::get_state ()
{
	Metrics::const_iterator i;
	XMLNode *root = new XMLNode ("TempoMap");

	{
		Glib::Threads::RWLock::ReaderLock lm (lock);
		for (i = _metrics.begin(); i != _metrics.end(); ++i) {
			root->add_child_nocopy ((*i)->get_state());
		}
	}

	return *root;
}

int
TempoMap::set_state (const XMLNode& node, int /*version*/)
{
	{
		Glib::Threads::RWLock::WriterLock lm (lock);

		XMLNodeList nlist;
		XMLNodeConstIterator niter;
		Metrics old_metrics (_metrics);
		_metrics.clear();

		nlist = node.children();

		for (niter = nlist.begin(); niter != nlist.end(); ++niter) {
			XMLNode* child = *niter;

			if (child->name() == TempoSection::xml_state_node_name) {

				try {
					TempoSection* ts = new TempoSection (*child);
					_metrics.push_back (ts);
				}

				catch (failed_constructor& err){
					error << _("Tempo map: could not set new state, restoring old one.") << endmsg;
					_metrics = old_metrics;
					old_metrics.clear();
					break;
				}

			} else if (child->name() == MeterSection::xml_state_node_name) {

				try {
					MeterSection* ms = new MeterSection (*child);
					_metrics.push_back (ms);
				}

				catch (failed_constructor& err) {
					error << _("Tempo map: could not set new state, restoring old one.") << endmsg;
					_metrics = old_metrics;
					old_metrics.clear();
					break;
				}
			}
		}

		if (niter == nlist.end()) {
			MetricSectionSorter cmp;
			_metrics.sort (cmp);
		}

		/* check for legacy sessions where bbt was the base musical unit for tempo */
		for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
			TempoSection* t;
			if ((t = dynamic_cast<TempoSection*> (*i)) != 0) {
				if (t->legacy_bbt().bars != 0) {
					fix_legacy_session();
					break;
				}
				break;
			}
		}

		/* check for multiple tempo/meters at the same location, which
		   ardour2 somehow allowed.
		*/

		Metrics::iterator prev = _metrics.end();
		for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
			if (prev != _metrics.end()) {
				MeterSection* ms;
				MeterSection* prev_m;
				TempoSection* ts;
				TempoSection* prev_t;
				if ((prev_m = dynamic_cast<MeterSection*>(*prev)) != 0 && (ms = dynamic_cast<MeterSection*>(*i)) != 0) {
					if (prev_m->pulse() == ms->pulse()) {
						cerr << string_compose (_("Multiple meter definitions found at %1"), prev_m->pulse()) << endmsg;
						error << string_compose (_("Multiple meter definitions found at %1"), prev_m->pulse()) << endmsg;
						return -1;
					}
				} else if ((prev_t = dynamic_cast<TempoSection*>(*prev)) != 0 && (ts = dynamic_cast<TempoSection*>(*i)) != 0) {
					if (prev_t->pulse() == ts->pulse()) {
						cerr << string_compose (_("Multiple tempo definitions found at %1"), prev_t->pulse()) << endmsg;
						error << string_compose (_("Multiple tempo definitions found at %1"), prev_t->pulse()) << endmsg;
						return -1;
					}
				}
			}
			prev = i;
		}

		recompute_map (_metrics);

		Metrics::const_iterator d = old_metrics.begin();
		while (d != old_metrics.end()) {
			delete (*d);
			++d;
		}
		old_metrics.clear ();
	}

	PropertyChanged (PropertyChange ());

	return 0;
}

void
TempoMap::dump (const Metrics& metrics, std::ostream& o) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock, Glib::Threads::TRY_LOCK);
	const MeterSection* m;
	const TempoSection* t;
	const TempoSection* prev_t = 0;

	for (Metrics::const_iterator i = metrics.begin(); i != metrics.end(); ++i) {

		if ((t = dynamic_cast<const TempoSection*>(*i)) != 0) {
			o << "Tempo @ " << *i << t->beats_per_minute() << " BPM (pulse = 1/" << t->note_type() << ") at " << t->pulse() << " frame= " << t->frame() << " (movable? "
			  << t->movable() << ')' << " pos lock: " << enum_2_string (t->position_lock_style()) << std::endl;
			o << "current      : " << t->beats_per_minute() << " | " << t->pulse() << " | " << t->frame() << std::endl;
			if (prev_t) {
				o << "previous     : " << prev_t->beats_per_minute() << " | " << prev_t->pulse() << " | " << prev_t->frame() << std::endl;
				o << "calculated   : " << prev_t->tempo_at_pulse (t->pulse()) *  prev_t->note_type() << " | " << prev_t->pulse_at_tempo (t->pulses_per_minute(), t->frame(), _frame_rate) <<  " | " << prev_t->frame_at_tempo (t->pulses_per_minute(), t->pulse(), _frame_rate) << std::endl;
			}
			prev_t = t;
		} else if ((m = dynamic_cast<const MeterSection*>(*i)) != 0) {
			o << "Meter @ " << *i << ' ' << m->divisions_per_bar() << '/' << m->note_divisor() << " at " << m->bbt() << " frame= " << m->frame()
			  << " pulse: " << m->pulse() <<  " beat : " << m->beat() << " pos lock: " << enum_2_string (m->position_lock_style()) << " (movable? " << m->movable() << ')' << endl;
		}
	}
	o << "------" << std::endl;
}

int
TempoMap::n_tempos() const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	int cnt = 0;

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if ((*i)->is_tempo()) {
			cnt++;
		}
	}

	return cnt;
}

int
TempoMap::n_meters() const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);
	int cnt = 0;

	for (Metrics::const_iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
		if (!(*i)->is_tempo()) {
			cnt++;
		}
	}

	return cnt;
}

void
TempoMap::insert_time (framepos_t where, framecnt_t amount)
{
	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
			if ((*i)->frame() >= where && (*i)->movable ()) {
				(*i)->set_frame ((*i)->frame() + amount);
			}
		}

		/* now reset the BBT time of all metrics, based on their new
		 * audio time. This is the only place where we do this reverse
		 * timestamp.
		 */

		Metrics::iterator i;
		const MeterSection* meter;
		const TempoSection* tempo;
		MeterSection *m;
		TempoSection *t;

		meter = &first_meter ();
		tempo = &first_tempo ();

		BBT_Time start;
		BBT_Time end;

		// cerr << "\n###################### TIMESTAMP via AUDIO ##############\n" << endl;

		bool first = true;
		MetricSection* prev = 0;

		for (i = _metrics.begin(); i != _metrics.end(); ++i) {

			BBT_Time bbt;
			//TempoMetric metric (*meter, *tempo);
			MeterSection* ms = const_cast<MeterSection*>(meter);
			TempoSection* ts = const_cast<TempoSection*>(tempo);
			if (prev) {
				if (ts){
					if ((t = dynamic_cast<TempoSection*>(prev)) != 0) {
						if (!t->active()) {
							continue;
						}
						ts->set_pulse (t->pulse());
					}
					if ((m = dynamic_cast<MeterSection*>(prev)) != 0) {
						ts->set_pulse (m->pulse());
					}
					ts->set_frame (prev->frame());

				}
				if (ms) {
					if ((m = dynamic_cast<MeterSection*>(prev)) != 0) {
						pair<double, BBT_Time> start = make_pair (m->beat(), m->bbt());
						ms->set_beat (start);
						ms->set_pulse (m->pulse());
					}
					if ((t = dynamic_cast<TempoSection*>(prev)) != 0) {
						if (!t->active()) {
							continue;
						}
						const double beat = beat_at_pulse_locked (_metrics, t->pulse());
						pair<double, BBT_Time> start = make_pair (beat, bbt_at_beat_locked (_metrics, beat));
						ms->set_beat (start);
						ms->set_pulse (t->pulse());
					}
					ms->set_frame (prev->frame());
				}

			} else {
				// metric will be at frames=0 bbt=1|1|0 by default
				// which is correct for our purpose
			}

			// cerr << bbt << endl;

			if ((t = dynamic_cast<TempoSection*>(*i)) != 0) {
				if (!t->active()) {
					continue;
				}
				t->set_pulse (pulse_at_frame_locked (_metrics, m->frame()));
				tempo = t;
				// cerr << "NEW TEMPO, frame = " << (*i)->frame() << " beat = " << (*i)->pulse() <<endl;
			} else if ((m = dynamic_cast<MeterSection*>(*i)) != 0) {
				bbt = bbt_at_frame_locked (_metrics, m->frame());

				// cerr << "timestamp @ " << (*i)->frame() << " with " << bbt.bars << "|" << bbt.beats << "|" << bbt.ticks << " => ";

				if (first) {
					first = false;
				} else {

					if (bbt.ticks > BBT_Time::ticks_per_beat/2) {
						/* round up to next beat */
						bbt.beats += 1;
					}

					bbt.ticks = 0;

					if (bbt.beats != 1) {
						/* round up to next bar */
						bbt.bars += 1;
						bbt.beats = 1;
					}
				}
				pair<double, BBT_Time> start = make_pair (max (0.0, beat_at_frame_locked (_metrics, m->frame())), bbt);
				m->set_beat (start);
				m->set_pulse (pulse_at_frame_locked (_metrics, m->frame()));
				meter = m;
				// cerr << "NEW METER, frame = " << (*i)->frame() << " beat = " << (*i)->pulse() <<endl;
			} else {
				fatal << _("programming error: unhandled MetricSection type") << endmsg;
				abort(); /*NOTREACHED*/
			}

			prev = (*i);
		}

		recompute_map (_metrics);
	}


	PropertyChanged (PropertyChange ());
}
bool
TempoMap::remove_time (framepos_t where, framecnt_t amount)
{
	bool moved = false;

	std::list<MetricSection*> metric_kill_list;

	TempoSection* last_tempo = NULL;
	MeterSection* last_meter = NULL;
	bool tempo_after = false; // is there a tempo marker at the first sample after the removed range?
	bool meter_after = false; // is there a meter marker likewise?
	{
		Glib::Threads::RWLock::WriterLock lm (lock);
		for (Metrics::iterator i = _metrics.begin(); i != _metrics.end(); ++i) {
			if ((*i)->frame() >= where && (*i)->frame() < where+amount) {
				metric_kill_list.push_back(*i);
				TempoSection *lt = dynamic_cast<TempoSection*> (*i);
				if (lt)
					last_tempo = lt;
				MeterSection *lm = dynamic_cast<MeterSection*> (*i);
				if (lm)
					last_meter = lm;
			}
			else if ((*i)->frame() >= where) {
				// TODO: make sure that moved tempo/meter markers are rounded to beat/bar boundaries
				(*i)->set_frame ((*i)->frame() - amount);
				if ((*i)->frame() == where) {
					// marker was immediately after end of range
					tempo_after = dynamic_cast<TempoSection*> (*i);
					meter_after = dynamic_cast<MeterSection*> (*i);
				}
				moved = true;
			}
		}

		//find the last TEMPO and METER metric (if any) and move it to the cut point so future stuff is correct
		if (last_tempo && !tempo_after) {
			metric_kill_list.remove(last_tempo);
			last_tempo->set_frame(where);
			moved = true;
		}
		if (last_meter && !meter_after) {
			metric_kill_list.remove(last_meter);
			last_meter->set_frame(where);
			moved = true;
		}

		//remove all the remaining metrics
		for (std::list<MetricSection*>::iterator i = metric_kill_list.begin(); i != metric_kill_list.end(); ++i) {
			_metrics.remove(*i);
			moved = true;
		}

		if (moved) {
			recompute_map (_metrics);
		}
	}
	PropertyChanged (PropertyChange ());
	return moved;
}

/** Add some (fractional) beats to a session frame position, and return the result in frames.
 *  pos can be -ve, if required.
 */
framepos_t
TempoMap::framepos_plus_beats (framepos_t frame, Evoral::Beats beats) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return frame_at_beat_locked (_metrics, beat_at_frame_locked (_metrics, frame) + beats.to_double());
}

/** Subtract some (fractional) beats from a frame position, and return the result in frames */
framepos_t
TempoMap::framepos_minus_beats (framepos_t pos, Evoral::Beats beats) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return frame_at_beat_locked (_metrics, beat_at_frame_locked (_metrics, pos) - beats.to_double());
}

/** Add the BBT interval op to pos and return the result */
framepos_t
TempoMap::framepos_plus_bbt (framepos_t pos, BBT_Time op) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	BBT_Time pos_bbt = bbt_at_beat_locked (_metrics, beat_at_frame_locked (_metrics, pos));
	pos_bbt.ticks += op.ticks;
	if (pos_bbt.ticks >= BBT_Time::ticks_per_beat) {
		++pos_bbt.beats;
		pos_bbt.ticks -= BBT_Time::ticks_per_beat;
	}
	pos_bbt.beats += op.beats;
	/* the meter in effect will start on the bar */
	double divisions_per_bar = meter_section_at_beat (beat_at_bbt_locked (_metrics, BBT_Time (pos_bbt.bars + op.bars, 1, 0))).divisions_per_bar();
	while (pos_bbt.beats >= divisions_per_bar + 1) {
		++pos_bbt.bars;
		divisions_per_bar = meter_section_at_beat (beat_at_bbt_locked (_metrics, BBT_Time (pos_bbt.bars + op.bars, 1, 0))).divisions_per_bar();
		pos_bbt.beats -= divisions_per_bar;
	}
	pos_bbt.bars += op.bars;

	return frame_at_bbt_locked (_metrics, pos_bbt);
}

/** Count the number of beats that are equivalent to distance when going forward,
    starting at pos.
*/
Evoral::Beats
TempoMap::framewalk_to_beats (framepos_t pos, framecnt_t distance) const
{
	Glib::Threads::RWLock::ReaderLock lm (lock);

	return Evoral::Beats (beat_at_frame_locked (_metrics, pos + distance) - beat_at_frame_locked (_metrics, pos));
}

struct bbtcmp {
    bool operator() (const BBT_Time& a, const BBT_Time& b) {
	    return a < b;
    }
};

std::ostream&
operator<< (std::ostream& o, const Meter& m) {
	return o << m.divisions_per_bar() << '/' << m.note_divisor();
}

std::ostream&
operator<< (std::ostream& o, const Tempo& t) {
	return o << t.beats_per_minute() << " 1/" << t.note_type() << "'s per minute";
}

std::ostream&
operator<< (std::ostream& o, const MetricSection& section) {

	o << "MetricSection @ " << section.frame() << ' ';

	const TempoSection* ts;
	const MeterSection* ms;

	if ((ts = dynamic_cast<const TempoSection*> (&section)) != 0) {
		o << *((const Tempo*) ts);
	} else if ((ms = dynamic_cast<const MeterSection*> (&section)) != 0) {
		o << *((const Meter*) ms);
	}

	return o;
}
