// Part of dump1090, a Mode S message decoder for RTLSDR devices.
//
// track.c: aircraft state tracking
//
// Copyright (c) 2014-2016 Oliver Jowett <oliver@mutability.co.uk>
//
// This file is free software: you may copy, redistribute and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 2 of the License, or (at your
// option) any later version.
//
// This file is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// This file incorporates work covered by the following copyright and
// permission notice:
//
//   Copyright (C) 2012 by Salvatore Sanfilippo <antirez@gmail.com>
//
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are
//   met:
//
//    *  Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//    *  Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "dump1090.h"
#include <inttypes.h>

/* #define DEBUG_CPR_CHECKS */

uint32_t modeAC_count[4096];
uint32_t modeAC_lastcount[4096];
uint32_t modeAC_match[4096];
uint32_t modeAC_age[4096];

//
// Return a new aircraft structure for the linked list of tracked
// aircraft
//
struct aircraft *trackCreateAircraft(struct modesMessage *mm) {
    static struct aircraft zeroAircraft;
    struct aircraft *a = (struct aircraft *) malloc(sizeof(*a));
    int i;

    // Default everything to zero/NULL
    *a = zeroAircraft;

    // Now initialise things that should not be 0/NULL to their defaults
    a->addr = mm->addr;
    a->addrtype = mm->addrtype;
    for (i = 0; i < 8; ++i)
        a->signalLevel[i] = 1e-5;
    a->signalNext = 0;

    // start off with the "last emitted" ACAS RA being blank (just the BDS 3,0
    // or ES type code)
    a->fatsv_emitted_bds_30[0] = 0x30;
    a->fatsv_emitted_es_acas_ra[0] = 0xE2;

    // defaults until we see an op status message
    a->adsb_version = -1;
    a->adsb_hrd = HEADING_MAGNETIC;
    a->adsb_tah = HEADING_GROUND_TRACK;

    // Copy the first message so we can emit it later when a second message arrives.
    a->first_message = *mm;

    // initialize data validity ages
#define F(f,s,e) do { a->f##_valid.stale_interval = (s) * 1000; a->f##_valid.expire_interval = (e) * 1000; } while (0)
    F(callsign,        60, 70);  // ADS-B or Comm-B
    F(altitude,        15, 70);  // ADS-B or Mode S
    F(altitude_geom,   60, 70);  // ADS-B only
    F(geom_delta,      60, 70);  // ADS-B only
    F(gs,              60, 70);  // ADS-B or Comm-B
    F(ias,             60, 70);  // ADS-B (rare) or Comm-B
    F(tas,             60, 70);  // ADS-B (rare) or Comm-B
    F(mach,            60, 70);  // Comm-B only
    F(track,           60, 70);  // ADS-B or Comm-B
    F(track_rate,      60, 70);  // Comm-B only
    F(roll,            60, 70);  // Comm-B only
    F(mag_heading,     60, 70);  // ADS-B (rare) or Comm-B
    F(true_heading,    60, 70);  // ADS-B only (rare)
    F(baro_rate,       60, 70);  // ADS-B or Comm-B
    F(geom_rate,       60, 70);  // ADS-B or Comm-B
    F(squawk,          15, 70);  // ADS-B or Mode S
    F(category,        60, 70);  // ADS-B only
    F(airground,       15, 70);  // ADS-B or Mode S
    F(alt_setting,     60, 70);  // Comm-B only
    F(intent_altitude, 60, 70);  // ADS-B or Comm-B
    F(intent_modes,    60, 70);  // ADS-B or Comm-B
    F(cpr_odd,         60, 70);  // ADS-B only
    F(cpr_even,        60, 70);  // ADS-B only
    F(position,        60, 70);  // ADS-B only
#undef F

    Modes.stats_current.unique_aircraft++;

    return (a);
}

//
//=========================================================================
//
// Return the aircraft with the specified address, or NULL if no aircraft
// exists with this address.
//
struct aircraft *trackFindAircraft(uint32_t addr) {
    struct aircraft *a = Modes.aircrafts;

    while(a) {
        if (a->addr == addr) return (a);
        a = a->next;
    }
    return (NULL);
}

// Should we accept some new data from the given source?
// If so, update the validity and return 1
static int accept_data(data_validity *d, datasource_t source)
{
    if (messageNow() < d->updated)
        return 0;

    if (source < d->source && messageNow() < d->stale)
        return 0;

    d->source = source;
    d->updated = messageNow();
    d->stale = messageNow() + d->stale_interval;
    d->expires = messageNow() + d->expire_interval;
    return 1;
}

// Given two datasources, produce a third datasource for data combined from them.
static void combine_validity(data_validity *to, const data_validity *from1, const data_validity *from2) {
    if (from1->source == SOURCE_INVALID) {
        *to = *from2;
        return;
    }

    if (from2->source == SOURCE_INVALID) {
        *to = *from1;
        return;
    }

    to->source = (from1->source < from2->source) ? from1->source : from2->source;        // the worse of the two input sources
    to->updated = (from1->updated > from2->updated) ? from1->updated : from2->updated;   // the *later* of the two update times
    to->stale = (from1->stale < from2->stale) ? from1->stale : from2->stale;             // the earlier of the two stale times
    to->expires = (from1->expires < from2->expires) ? from1->expires : from2->expires;   // the earlier of the two expiry times
}

static int compare_validity(const data_validity *lhs, const data_validity *rhs) {
    if (messageNow() < lhs->stale && lhs->source > rhs->source)
        return 1;
    else if (messageNow() < rhs->stale && lhs->source < rhs->source)
        return -1;
    else if (lhs->updated > rhs->updated)
        return 1;
    else if (lhs->updated < rhs->updated)
        return -1;
    else
        return 0;
}

//
// CPR position updating
//

// Distance between points on a spherical earth.
// This has up to 0.5% error because the earth isn't actually spherical
// (but we don't use it in situations where that matters)
static double greatcircle(double lat0, double lon0, double lat1, double lon1)
{
    double dlat, dlon;

    lat0 = lat0 * M_PI / 180.0;
    lon0 = lon0 * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lon1 = lon1 * M_PI / 180.0;

    dlat = fabs(lat1 - lat0);
    dlon = fabs(lon1 - lon0);

    // use haversine for small distances for better numerical stability
    if (dlat < 0.001 && dlon < 0.001) {
        double a = sin(dlat/2) * sin(dlat/2) + cos(lat0) * cos(lat1) * sin(dlon/2) * sin(dlon/2);
        return 6371e3 * 2 * atan2(sqrt(a), sqrt(1.0 - a));
    }

    // spherical law of cosines
    return 6371e3 * acos(sin(lat0) * sin(lat1) + cos(lat0) * cos(lat1) * cos(dlon));
}

static void update_range_histogram(double lat, double lon)
{
    if (Modes.stats_range_histo && (Modes.bUserFlags & MODES_USER_LATLON_VALID)) {
        double range = greatcircle(Modes.fUserLat, Modes.fUserLon, lat, lon);
        int bucket = round(range / Modes.maxRange * RANGE_BUCKET_COUNT);

        if (bucket < 0)
            bucket = 0;
        else if (bucket >= RANGE_BUCKET_COUNT)
            bucket = RANGE_BUCKET_COUNT-1;

        ++Modes.stats_current.range_histogram[bucket];
    }
}

// return true if it's OK for the aircraft to have travelled from its last known position
// to a new position at (lat,lon,surface) at a time of now.
static int speed_check(struct aircraft *a, double lat, double lon, int surface)
{
    uint64_t elapsed;
    double distance;
    double range;
    int speed;
    int inrange;

    if (!trackDataValid(&a->position_valid))
        return 1; // no reference, assume OK

    elapsed = trackDataAge(&a->position_valid);

    if (trackDataValid(&a->gs_valid))
        speed = a->gs;
    else if (trackDataValid(&a->tas_valid))
        speed = a->tas * 4 / 3;
    else if (trackDataValid(&a->ias_valid))
        speed = a->ias * 2;
    else
        speed = surface ? 100 : 600; // guess

    // Work out a reasonable speed to use:
    //  current speed + 1/3
    //  surface speed min 20kt, max 150kt
    //  airborne speed min 200kt, no max
    speed = speed * 4 / 3;
    if (surface) {
        if (speed < 20)
            speed = 20;
        if (speed > 150)
            speed = 150;
    } else {
        if (speed < 200)
            speed = 200;
    }

    // 100m (surface) or 500m (airborne) base distance to allow for minor errors,
    // plus distance covered at the given speed for the elapsed time + 1 second.
    range = (surface ? 0.1e3 : 0.5e3) + ((elapsed + 1000.0) / 1000.0) * (speed * 1852.0 / 3600.0);

    // find actual distance
    distance = greatcircle(a->lat, a->lon, lat, lon);

    inrange = (distance <= range);
#ifdef DEBUG_CPR_CHECKS
    if (!inrange) {
        fprintf(stderr, "Speed check failed: %06x: %.3f,%.3f -> %.3f,%.3f in %.1f seconds, max speed %d kt, range %.1fkm, actual %.1fkm\n",
                a->addr, a->lat, a->lon, lat, lon, elapsed/1000.0, speed, range/1000.0, distance/1000.0);
    }
#endif

    return inrange;
}

static int doGlobalCPR(struct aircraft *a, struct modesMessage *mm, double *lat, double *lon, unsigned *nuc)
{
    int result;
    int fflag = mm->cpr_odd;
    int surface = (mm->cpr_type == CPR_SURFACE);

    *nuc = (a->cpr_even_nuc < a->cpr_odd_nuc ? a->cpr_even_nuc : a->cpr_odd_nuc); // worst of the two positions

    if (surface) {
        // surface global CPR
        // find reference location
        double reflat, reflon;

        if (trackDataValid(&a->position_valid)) { // Ok to try aircraft relative first
            reflat = a->lat;
            reflon = a->lon;
            if (a->pos_nuc < *nuc)
                *nuc = a->pos_nuc;
        } else if (Modes.bUserFlags & MODES_USER_LATLON_VALID) {
            reflat = Modes.fUserLat;
            reflon = Modes.fUserLon;
        } else {
            // No local reference, give up
            return (-1);
        }

        result = decodeCPRsurface(reflat, reflon,
                                  a->cpr_even_lat, a->cpr_even_lon,
                                  a->cpr_odd_lat, a->cpr_odd_lon,
                                  fflag,
                                  lat, lon);
    } else {
        // airborne global CPR
        result = decodeCPRairborne(a->cpr_even_lat, a->cpr_even_lon,
                                   a->cpr_odd_lat, a->cpr_odd_lon,
                                   fflag,
                                   lat, lon);
    }

    if (result < 0) {
#ifdef DEBUG_CPR_CHECKS
        fprintf(stderr, "CPR: decode failure for %06X (%d).\n", a->addr, result);
        fprintf(stderr, "  even: %d %d   odd: %d %d  fflag: %s\n",
                a->cpr_even_lat, a->cpr_even_lon,
                a->cpr_odd_lat, a->cpr_odd_lon,
                fflag ? "odd" : "even");
#endif
        return result;
    }

    // check max range
    if (Modes.maxRange > 0 && (Modes.bUserFlags & MODES_USER_LATLON_VALID)) {
        double range = greatcircle(Modes.fUserLat, Modes.fUserLon, *lat, *lon);
        if (range > Modes.maxRange) {
#ifdef DEBUG_CPR_CHECKS
            fprintf(stderr, "Global range check failed: %06x: %.3f,%.3f, max range %.1fkm, actual %.1fkm\n",
                    a->addr, *lat, *lon, Modes.maxRange/1000.0, range/1000.0);
#endif

            Modes.stats_current.cpr_global_range_checks++;
            return (-2); // we consider an out-of-range value to be bad data
        }
    }

    // for mlat results, skip the speed check
    if (mm->source == SOURCE_MLAT)
        return result;

    // check speed limit
    if (trackDataValid(&a->position_valid) && a->pos_nuc >= *nuc && !speed_check(a, *lat, *lon, surface)) {
        Modes.stats_current.cpr_global_speed_checks++;
        return -2;
    }

    return result;
}

static int doLocalCPR(struct aircraft *a, struct modesMessage *mm, double *lat, double *lon, unsigned *nuc)
{
    // relative CPR
    // find reference location
    double reflat, reflon;
    double range_limit = 0;
    int result;
    int fflag = mm->cpr_odd;
    int surface = (mm->cpr_type == CPR_SURFACE);

    *nuc = mm->cpr_nucp;

    if (trackDataValid(&a->position_valid)) {
        reflat = a->lat;
        reflon = a->lon;

        if (a->pos_nuc < *nuc)
            *nuc = a->pos_nuc;

        range_limit = 50e3;
    } else if (!surface && (Modes.bUserFlags & MODES_USER_LATLON_VALID)) {
        reflat = Modes.fUserLat;
        reflon = Modes.fUserLon;

        // The cell size is at least 360NM, giving a nominal
        // max range of 180NM (half a cell).
        //
        // If the receiver range is more than half a cell
        // then we must limit this range further to avoid
        // ambiguity. (e.g. if we receive a position report
        // at 200NM distance, this may resolve to a position
        // at (200-360) = 160NM in the wrong direction)

        if (Modes.maxRange == 0) {
            return (-1); // Can't do receiver-centered checks at all
        } else if (Modes.maxRange <= 1852*180) {
            range_limit = Modes.maxRange;
        } else if (Modes.maxRange < 1852*360) {
            range_limit = (1852*360) - Modes.maxRange;
        } else {
            return (-1); // Can't do receiver-centered checks at all
        }
    } else {
        // No local reference, give up
        return (-1);
    }

    result = decodeCPRrelative(reflat, reflon,
                               mm->cpr_lat,
                               mm->cpr_lon,
                               fflag, surface,
                               lat, lon);
    if (result < 0) {
        return result;
    }

    // check range limit
    if (range_limit > 0) {
        double range = greatcircle(reflat, reflon, *lat, *lon);
        if (range > range_limit) {
            Modes.stats_current.cpr_local_range_checks++;
            return (-1);
        }
    }

    // check speed limit
    if (trackDataValid(&a->position_valid) && a->pos_nuc >= *nuc && !speed_check(a, *lat, *lon, surface)) {
#ifdef DEBUG_CPR_CHECKS
        fprintf(stderr, "Speed check for %06X with local decoding failed\n", a->addr);
#endif
        Modes.stats_current.cpr_local_speed_checks++;
        return -1;
    }

    return 0;
}

static uint64_t time_between(uint64_t t1, uint64_t t2)
{
    if (t1 >= t2)
        return t1 - t2;
    else
        return t2 - t1;
}

static void updatePosition(struct aircraft *a, struct modesMessage *mm)
{
    int location_result = -1;
    uint64_t max_elapsed;
    double new_lat = 0, new_lon = 0;
    unsigned new_nuc = 0;
    int surface;

    surface = (mm->cpr_type == CPR_SURFACE);

    if (surface) {
        ++Modes.stats_current.cpr_surface;

        // Surface: 25 seconds if >25kt or speed unknown, 50 seconds otherwise
        if (mm->gs_valid && mm->gs <= 25)
            max_elapsed = 50000;
        else
            max_elapsed = 25000;
    } else {
        ++Modes.stats_current.cpr_airborne;

        // Airborne: 10 seconds
        max_elapsed = 10000;
    }

    // If we have enough recent data, try global CPR
    if (trackDataValid(&a->cpr_odd_valid) && trackDataValid(&a->cpr_even_valid) &&
        a->cpr_odd_valid.source == a->cpr_even_valid.source &&
        a->cpr_odd_type == a->cpr_even_type &&
        time_between(a->cpr_odd_valid.updated, a->cpr_even_valid.updated) <= max_elapsed) {

        location_result = doGlobalCPR(a, mm, &new_lat, &new_lon, &new_nuc);

        if (location_result == -2) {
#ifdef DEBUG_CPR_CHECKS
            fprintf(stderr, "global CPR failure (invalid) for (%06X).\n", a->addr);
#endif
            // Global CPR failed because the position produced implausible results.
            // This is bad data. Discard both odd and even messages and wait for a fresh pair.
            // Also disable aircraft-relative positions until we have a new good position (but don't discard the
            // recorded position itself)
            Modes.stats_current.cpr_global_bad++;
            a->cpr_odd_valid.source = a->cpr_even_valid.source = a->position_valid.source = SOURCE_INVALID;

            return;
        } else if (location_result == -1) {
#ifdef DEBUG_CPR_CHECKS
            if (mm->source == SOURCE_MLAT) {
                fprintf(stderr, "CPR skipped from MLAT (%06X).\n", a->addr);
            }
#endif
            // No local reference for surface position available, or the two messages crossed a zone.
            // Nonfatal, try again later.
            Modes.stats_current.cpr_global_skipped++;
        } else {
            Modes.stats_current.cpr_global_ok++;
            combine_validity(&a->position_valid, &a->cpr_even_valid, &a->cpr_odd_valid);
        }
    }

    // Otherwise try relative CPR.
    if (location_result == -1) {
        location_result = doLocalCPR(a, mm, &new_lat, &new_lon, &new_nuc);

        if (location_result < 0) {
            Modes.stats_current.cpr_local_skipped++;
        } else {
            Modes.stats_current.cpr_local_ok++;
            mm->cpr_relative = 1;

            if (mm->cpr_odd) {
                a->position_valid = a->cpr_odd_valid;
            } else {
                a->position_valid = a->cpr_even_valid;
            }
        }
    }

    if (location_result == 0) {
        // If we sucessfully decoded, back copy the results to mm so that we can print them in list output
        mm->cpr_decoded = 1;
        mm->decoded_lat = new_lat;
        mm->decoded_lon = new_lon;

        // Update aircraft state
        a->lat = new_lat;
        a->lon = new_lon;
        a->pos_nuc = new_nuc;

        update_range_histogram(new_lat, new_lon);
    }
}

//
//=========================================================================
//
// Receive new messages and update tracked aircraft state
//

struct aircraft *trackUpdateFromMessage(struct modesMessage *mm)
{
    struct aircraft *a;

    if (mm->msgtype == 32) {
        // Mode A/C, just count it (we ignore SPI)
        modeAC_count[modeAToIndex(mm->squawk)]++;
        return NULL;
    }

    _messageNow = mm->sysTimestampMsg;
    
    // Lookup our aircraft or create a new one
    a = trackFindAircraft(mm->addr);
    if (!a) {                              // If it's a currently unknown aircraft....
        a = trackCreateAircraft(mm);       // ., create a new record for it,
        a->next = Modes.aircrafts;         // .. and put it at the head of the list
        Modes.aircrafts = a;
    }

    if (mm->signalLevel > 0) {
        a->signalLevel[a->signalNext] = mm->signalLevel;
        a->signalNext = (a->signalNext + 1) & 7;
    }
    a->seen      = messageNow();
    a->messages++;

    // update addrtype, we only ever go towards "more direct" types
    if (mm->addrtype < a->addrtype)
        a->addrtype = mm->addrtype;

    // if we saw some direct ADS-B for the first time, assume version 0
    if (mm->source == SOURCE_ADSB && a->adsb_version < 0)
        a->adsb_version = 0;

    if (mm->altitude_valid && mm->altitude_source == ALTITUDE_BARO && accept_data(&a->altitude_valid, mm->source)) {
        if (a->modeC_hit) {
            int new_modeC = (a->altitude + 49) / 100;
            int old_modeC = (mm->altitude + 49) / 100;
            if (new_modeC != old_modeC) {
                a->modeC_hit = 0;
            }
        }

        a->altitude = mm->altitude;
    }

    if (mm->squawk_valid && accept_data(&a->squawk_valid, mm->source)) {
        if (mm->squawk != a->squawk) {
            a->modeA_hit = 0;
        }
        a->squawk = mm->squawk;
    }

    if (mm->altitude_valid && mm->altitude_source == ALTITUDE_GEOM && accept_data(&a->altitude_geom_valid, mm->source)) {
        a->altitude_geom = mm->altitude;
    }

    if (mm->geom_delta_valid && accept_data(&a->geom_delta_valid, mm->source)) {
        a->geom_delta = mm->geom_delta;
    }

    if (mm->heading_valid) {
        heading_type_t htype = mm->heading_type;
        if (htype == HEADING_MAGNETIC_OR_TRUE) {
            htype = a->adsb_hrd;
        } else if (htype == HEADING_TRACK_OR_HEADING) {
            htype = a->adsb_tah;
        }

        if (htype == HEADING_GROUND_TRACK && accept_data(&a->track_valid, mm->source)) {
            a->track = mm->heading;
        } else if (htype == HEADING_MAGNETIC && accept_data(&a->mag_heading_valid, mm->source)) {
            a->mag_heading = mm->heading;
        } else if (htype == HEADING_TRUE && accept_data(&a->true_heading_valid, mm->source)) {
            a->true_heading = mm->heading;
        }
    }

    if (mm->track_rate_valid && accept_data(&a->track_rate_valid, mm->source)) {
        a->track_rate = mm->track_rate;
    }

    if (mm->roll_valid && accept_data(&a->roll_valid, mm->source)) {
        a->roll = mm->roll;
    }

    if (mm->gs_valid && accept_data(&a->gs_valid, mm->source)) {
        a->gs = mm->gs;
    }

    if (mm->ias_valid && accept_data(&a->ias_valid, mm->source)) {
        a->ias = mm->ias;
    }

    if (mm->tas_valid && accept_data(&a->tas_valid, mm->source)) {
        a->tas = mm->tas;
    }

    if (mm->mach_valid && accept_data(&a->mach_valid, mm->source)) {
        a->mach = mm->mach;
    }

    if (mm->baro_rate_valid && accept_data(&a->baro_rate_valid, mm->source)) {
        a->baro_rate = mm->baro_rate;
    }

    if (mm->geom_rate_valid && accept_data(&a->geom_rate_valid, mm->source)) {
        a->geom_rate = mm->geom_rate;
    }

    if (mm->category_valid && accept_data(&a->category_valid, mm->source)) {
        a->category = mm->category;
    }

    if (mm->airground != AG_INVALID && accept_data(&a->airground_valid, mm->source)) {
        a->airground = mm->airground;
    }

    if (mm->callsign_valid && accept_data(&a->callsign_valid, mm->source)) {
        memcpy(a->callsign, mm->callsign, sizeof(a->callsign));
    }

    // prefer MCP over FMS
    // unless the source says otherwise
    if (mm->intent.mcp_altitude_valid && mm->intent.altitude_source != INTENT_ALT_FMS && accept_data(&a->intent_altitude_valid, mm->source)) {
        a->intent_altitude = mm->intent.mcp_altitude;
    } else if (mm->intent.fms_altitude_valid && accept_data(&a->intent_altitude_valid, mm->source)) {
        a->intent_altitude = mm->intent.fms_altitude;
    }

    if (mm->intent.heading_valid && accept_data(&a->intent_heading_valid, mm->source)) {
        a->intent_heading = mm->intent.heading;
    }

    if (mm->intent.modes_valid && accept_data(&a->intent_modes_valid, mm->source)) {
        a->intent_modes = mm->intent.modes;
    }

    if (mm->intent.alt_setting_valid && accept_data(&a->alt_setting_valid, mm->source)) {
        a->alt_setting = mm->intent.alt_setting;
    }

    // CPR, even
    if (mm->cpr_valid && !mm->cpr_odd && accept_data(&a->cpr_even_valid, mm->source)) {
        a->cpr_even_type = mm->cpr_type;
        a->cpr_even_lat = mm->cpr_lat;
        a->cpr_even_lon = mm->cpr_lon;
        a->cpr_even_nuc = mm->cpr_nucp;
    }

    // CPR, odd
    if (mm->cpr_valid && mm->cpr_odd && accept_data(&a->cpr_odd_valid, mm->source)) {
        a->cpr_odd_type = mm->cpr_type;
        a->cpr_odd_lat = mm->cpr_lat;
        a->cpr_odd_lon = mm->cpr_lon;
        a->cpr_odd_nuc = mm->cpr_nucp;
    }

    // operational status message
    if (mm->opstatus.valid) {
        a->adsb_version = mm->opstatus.version;
        if (mm->opstatus.version > 0) {
            a->adsb_hrd = mm->opstatus.hrd;
            a->adsb_tah = mm->opstatus.tah;
        }
    }

    // Now handle derived data

    // derive geometric altitude if we have baro + delta
    if (compare_validity(&a->altitude_valid, &a->altitude_geom_valid) > 0 &&
        compare_validity(&a->geom_delta_valid, &a->altitude_geom_valid) > 0) {
        // Baro and delta are both more recent than geometric, derive geometric from baro + delta
        a->altitude_geom = a->altitude + a->geom_delta;
        combine_validity(&a->altitude_geom_valid, &a->altitude_valid, &a->geom_delta_valid);
    }

    // If we've got a new cprlat or cprlon
    if (mm->cpr_valid) {
        updatePosition(a, mm);
    }

    return (a);
}

//
// Periodic updates of tracking state
//

// Periodically match up mode A/C results with mode S results
static void trackMatchAC(uint64_t now)
{
    // clear match flags
    for (unsigned i = 0; i < 4096; ++i) {
        modeAC_match[i] = 0;
    }

    // scan aircraft list, look for matches
    for (struct aircraft *a = Modes.aircrafts; a; a = a->next) {
        if ((now - a->seen) > 5000) {
            continue;
        }

        // match on Mode A
        if (trackDataValid(&a->squawk_valid)) {
            unsigned i = modeAToIndex(a->squawk);
            if ((modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                a->modeA_hit = 1;
                modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
            }
        }

        // match on Mode C (+/- 100ft)
        if (trackDataValid(&a->altitude_valid)) {
            int modeC = (a->altitude + 49) / 100;

            unsigned modeA = modeCToModeA(modeC);
            unsigned i = modeAToIndex(modeA);
            if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                a->modeC_hit = 1;
                modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
            }

            modeA = modeCToModeA(modeC + 1);
            i = modeAToIndex(modeA);
            if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                a->modeC_hit = 1;
                modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
            }

            modeA = modeCToModeA(modeC - 1);
            i = modeAToIndex(modeA);
            if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES) {
                a->modeC_hit = 1;
                modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
            }
        }
    }

    // reset counts for next time
    for (unsigned i = 0; i < 4096; ++i) {
        if (!modeAC_count[i])
            continue;

        if ((modeAC_count[i] - modeAC_lastcount[i]) < TRACK_MODEAC_MIN_MESSAGES) {
            if (++modeAC_age[i] > 15) {
                // not heard from for a while, clear it out
                modeAC_lastcount[i] = modeAC_count[i] = modeAC_age[i] = 0;
            }
        } else {
            // this one is live
            // set a high initial age for matches, so they age out rapidly
            // and don't show up on the interactive display when the matching
            // mode S data goes away or changes
            if (modeAC_match[i]) {
                modeAC_age[i] = 10;
            } else {
                modeAC_age[i] = 0;
            }
        }

        modeAC_lastcount[i] = modeAC_count[i];
    }
}

//
//=========================================================================
//
// If we don't receive new nessages within TRACK_AIRCRAFT_TTL
// we remove the aircraft from the list.
//
static void trackRemoveStaleAircraft(uint64_t now)
{
    struct aircraft *a = Modes.aircrafts;
    struct aircraft *prev = NULL;

    while(a) {
        if ((now - a->seen) > TRACK_AIRCRAFT_TTL ||
            (a->messages == 1 && (now - a->seen) > TRACK_AIRCRAFT_ONEHIT_TTL)) {
            // Count aircraft where we saw only one message before reaping them.
            // These are likely to be due to messages with bad addresses.
            if (a->messages == 1)
                Modes.stats_current.single_message_aircraft++;

            // Remove the element from the linked list, with care
            // if we are removing the first element
            if (!prev) {
                Modes.aircrafts = a->next; free(a); a = Modes.aircrafts;
            } else {
                prev->next = a->next; free(a); a = prev->next;
            }
        } else {

#define EXPIRE(_f) do { if (a->_f##_valid.source != SOURCE_INVALID && now >= a->_f##_valid.expires) { a->_f##_valid.source = SOURCE_INVALID; } } while (0)
            EXPIRE(callsign);
            EXPIRE(altitude);
            EXPIRE(altitude_geom);
            EXPIRE(geom_delta);
            EXPIRE(gs);
            EXPIRE(ias);
            EXPIRE(tas);
            EXPIRE(mach);
            EXPIRE(track);
            EXPIRE(track_rate);
            EXPIRE(roll);
            EXPIRE(mag_heading);
            EXPIRE(true_heading);
            EXPIRE(baro_rate);
            EXPIRE(geom_rate);
            EXPIRE(squawk);
            EXPIRE(category);
            EXPIRE(airground);
            EXPIRE(alt_setting);
            EXPIRE(intent_altitude);
            EXPIRE(intent_heading);
            EXPIRE(intent_modes);
            EXPIRE(cpr_odd);
            EXPIRE(cpr_even);
            EXPIRE(position);
#undef EXPIRE
            prev = a; a = a->next;
        }
    }
}


//
// Entry point for periodic updates
//

void trackPeriodicUpdate()
{
    static uint64_t next_update;
    uint64_t now = mstime();

    // Only do updates once per second
    if (now >= next_update) {
        next_update = now + 1000;
        trackRemoveStaleAircraft(now);
        trackMatchAC(now);
    }
}
