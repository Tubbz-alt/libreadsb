#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "cpr.h"
#include "geomag.h"
#include "mode_ac.h"
#include "track.h"

uint32_t modeAC_count[4096];
uint32_t modeAC_lastcount[4096];
uint32_t modeAC_match[4096];
uint32_t modeAC_age[4096];

/* Return a new aircraft structure for the linked list of tracked aircraft.
 */
static struct aircraft *track_create_aircraft(modes_message_t *mm)
{
    static struct aircraft zeroAircraft;
    struct aircraft *a = (struct aircraft *)malloc(sizeof(*a));
    int i;

    // Default everything to zero/NULL
    *a = zeroAircraft;

    // Now initialise things that should not be 0/NULL to their defaults
    a->addr = mm->addr;
    a->addr_type = mm->addrtype;
    for (i = 0; i < 8; ++i)
        a->signalLevel[i] = 1e-5;
    a->signalNext = 0;

    // defaults until we see a message otherwise
    a->adsb_version = -1;
    a->adsb_hrd = HEADING_MAGNETIC;
    a->adsb_tah = HEADING_GROUND_TRACK;
    // Copy the first message so we can emit it later when a second message arrives.
    a->first_message = *mm;

    // initialize data validity ages
#define F(f, s, e)                               \
    do                                           \
    {                                            \
        a->f##_valid.stale_interval = (s)*1000;  \
        a->f##_valid.expire_interval = (e)*1000; \
    } while (0)
    F(callsign, 60, 70);         // ADS-B or Comm-B
    F(altitude_baro, 15, 70);    // ADS-B or Mode S
    F(altitude_geom, 60, 70);    // ADS-B only
    F(geom_delta, 60, 70);       // ADS-B only
    F(gs, 60, 70);               // ADS-B or Comm-B
    F(ias, 60, 70);              // ADS-B (rare) or Comm-B
    F(tas, 60, 70);              // ADS-B (rare) or Comm-B
    F(mach, 60, 70);             // Comm-B only
    F(track, 60, 70);            // ADS-B or Comm-B
    F(track_rate, 60, 70);       // Comm-B only
    F(roll, 60, 70);             // Comm-B only
    F(mag_heading, 60, 70);      // ADS-B (rare) or Comm-B
    F(true_heading, 60, 70);     // ADS-B only (rare)
    F(baro_rate, 60, 70);        // ADS-B or Comm-B
    F(geom_rate, 60, 70);        // ADS-B or Comm-B
    F(squawk, 15, 70);           // ADS-B or Mode S
    F(airground, 15, 70);        // ADS-B or Mode S
    F(nav_qnh, 60, 70);          // Comm-B only
    F(nav_altitude_mcp, 60, 70); // ADS-B or Comm-B
    F(nav_altitude_fms, 60, 70); // ADS-B or Comm-B
    F(nav_altitude_src, 60, 70); // ADS-B or Comm-B
    F(nav_heading, 60, 70);      // ADS-B or Comm-B
    F(nav_modes, 60, 70);        // ADS-B or Comm-B
    F(cpr_odd, 60, 70);          // ADS-B only
    F(cpr_even, 60, 70);         // ADS-B only
    F(position, 60, 70);         // ADS-B only
    F(nic_a, 60, 70);            // ADS-B only
    F(nic_c, 60, 70);            // ADS-B only
    F(nic_baro, 60, 70);         // ADS-B only
    F(nac_p, 60, 70);            // ADS-B only
    F(nac_v, 60, 70);            // ADS-B only
    F(sil, 60, 70);              // ADS-B only
    F(gva, 60, 70);              // ADS-B only
    F(sda, 60, 70);              // ADS-B only
#undef F

    lib_state.stats_current.unique_aircraft++;

    return (a);
}

/* Return the aircraft with the specified address, or NULL if no aircraft
 * exists with this address.
 */
static struct aircraft *track_find_aircraft(uint32_t addr)
{
    struct aircraft *a = lib_state.aircrafts[addr % AIRCRAFTS_BUCKETS];

    while (a)
    {
        if (a->addr == addr)
            return (a);
        a = a->next;
    }
    return (NULL);
}

/* Should we accept some new data from the given source?
 * If so, update the validity and return 1
 */
static int accept_data(data_validity *d, datasource_t source, modes_message_t *mm, int reduce_often)
{
    if (messageNow() < d->updated)
        return 0;

    if (source < d->source && messageNow() < d->stale)
        return 0;

    d->source = source;
    d->updated = messageNow();
    d->stale = messageNow() + (d->stale_interval ? d->stale_interval : 60000);
    d->expires = messageNow() + (d->expire_interval ? d->expire_interval : 70000);

    if (messageNow() > d->next_reduce_forward && !mm->sbs_in)
    {
        // make sure global CPR stays possible even at high interval:
        if (mm->cpr_valid)
        {
            d->next_reduce_forward = messageNow() + 7000;
        }
        mm->reduce_forward = 1;
    }

    return 1;
}

/* Given two datasources, produce a third datasource for data combined from them.
 */
static void combine_validity(data_validity *to, const data_validity *from1, const data_validity *from2)
{
    if (from1->source == SOURCE_INVALID)
    {
        *to = *from2;
        return;
    }

    if (from2->source == SOURCE_INVALID)
    {
        *to = *from1;
        return;
    }

    to->source = (from1->source < from2->source) ? from1->source : from2->source;      // the worse of the two input sources
    to->updated = (from1->updated > from2->updated) ? from1->updated : from2->updated; // the *later* of the two update times
    to->stale = (from1->stale < from2->stale) ? from1->stale : from2->stale;           // the earlier of the two stale times
    to->expires = (from1->expires < from2->expires) ? from1->expires : from2->expires; // the earlier of the two expiry times
}

static int compare_validity(const data_validity *lhs, const data_validity *rhs)
{
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

/**
 * Calculate bearing of two coordinates
 * @param lat0 Latitude start
 * @param lon0 Longitude start
 * @param lat1 Latitude end
 * @param lon1 Longitude end
 * @return Bearing in 0-360 degree.
 */
static double get_bearing(double lat0, double lon0, double lat1, double lon1)
{
    lat0 = lat0 * M_PI / 180.0;
    lon0 = lon0 * M_PI / 180.0;
    lat1 = lat1 * M_PI / 180.0;
    lon1 = lon1 * M_PI / 180.0;

    double dlon = lon1 - lon0;
    double x = cos(lat0) * sin(dlon);
    double y = cos(lat1) * sin(lat0) - sin(lat1) * cos(lat0) * cos(dlon);
    double b = atan2(x, y);
    double bearing = 180 / M_PI * b;
    return bearing + 180;
}

/* CPR position updating
 *
 * Distance between points on a spherical earth.
 * This has up to 0.5% error because the earth isn't actually spherical
 * (but we don't use it in situations where that matters)
 */
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
    if (dlat < 0.001 && dlon < 0.001)
    {
        double a = sin(dlat / 2) * sin(dlat / 2) + cos(lat0) * cos(lat1) * sin(dlon / 2) * sin(dlon / 2);
        return 6371e3 * 2 * atan2(sqrt(a), sqrt(1.0 - a));
    }

    // spherical law of cosines
    return 6371e3 * acos(sin(lat0) * sin(lat1) + cos(lat0) * cos(lat1) * cos(dlon));
}

static uint32_t update_polar_range(double lat, double lon)
{
    double range = 0;
    int valid_latlon = lib_state.bUserFlags & MODES_USER_LATLON_VALID;

    if (!valid_latlon)
        return 0;

    range = greatcircle(lib_state.config.latitude, lib_state.config.longitude, lat, lon);

    if ((range <= lib_state.config.max_range || lib_state.config.max_range == 0) && range > lib_state.stats_current.longest_distance)
    {
        lib_state.stats_current.longest_distance = range;
    }

    // Round bearing to polarplot resolution.
    int bucket = round(get_bearing(lib_state.config.latitude, lib_state.config.longitude, lat, lon) / POLAR_RANGE_RESOLUTION);
    // Catch and avoid out of bounds writes
    if (bucket >= POLAR_RANGE_BUCKETS)
    {
        bucket = 0;
    }

    if (bucket < POLAR_RANGE_BUCKETS && lib_state.stats_range.polar_range[bucket] < range)
    {
        lib_state.stats_range.polar_range[bucket] = (uint32_t)range;
    }

    return (uint32_t)range;
}

/* Return true if it's OK for the aircraft to have travelled from its last known position
 * to a new position at (lat,lon,surface) at a time of now.
 */
static int speed_check(struct aircraft *a, double lat, double lon, int surface)
{
    uint64_t elapsed;
    double distance;
    double range;
    int speed;
    int inrange;

    if (!track_data_valid(&a->position_valid))
        return 1; // no reference, assume OK

    elapsed = track_data_age(&a->position_valid);

    if (track_data_valid(&a->gs_valid))
    {
        // use the larger of the current and earlier speed
        speed = (a->gs_last_pos > a->gs) ? a->gs_last_pos : a->gs;
        // add 2 knots for every second we haven't known the speed
        speed = speed + (2 * track_data_age(&a->gs_valid) / 1000.0);
    }
    else if (track_data_valid(&a->tas_valid))
    {
        speed = a->tas * 4 / 3;
    }
    else if (track_data_valid(&a->ias_valid))
    {
        speed = a->ias * 2;
    }
    else
    {
        speed = surface ? 100 : 700; // guess
    }

    // Work out a reasonable speed to use:
    //  current speed + 1/3
    //  surface speed min 20kt, max 150kt
    //  airborne speed min 200kt, no max
    speed = speed * 4 / 3;
    if (surface)
    {
        if (speed < 20)
            speed = 20;
        if (speed > 150)
            speed = 150;
    }
    else
    {
        if (speed < 200)
            speed = 200;
    }

    // 100m (surface) or 500m (airborne) base distance to allow for minor errors,
    // plus distance covered at the given speed for the elapsed time + 1 second.
    range = (surface ? 0.1e3 : 0.5e3) + ((elapsed + 1000.0) / 1000.0) * (speed * 1852.0 / 3600.0);

    // find actual distance
    distance = greatcircle(a->lat, a->lon, lat, lon);

    inrange = (distance <= range);

    return inrange;
}

static int do_global_cpr(struct aircraft *a, modes_message_t *mm, double *lat, double *lon, unsigned *nic, unsigned *rc)
{
    int result;
    int fflag = mm->cpr_odd;
    int surface = (mm->cpr_type == CPR_SURFACE);

    // derive NIC, Rc from the worse of the two position
    // smaller NIC is worse; larger Rc is worse
    *nic = (a->cpr_even_nic < a->cpr_odd_nic ? a->cpr_even_nic : a->cpr_odd_nic);
    *rc = (a->cpr_even_rc > a->cpr_odd_rc ? a->cpr_even_rc : a->cpr_odd_rc);

    if (surface)
    {
        // surface global CPR
        // find reference location
        double reflat, reflon;

        if (track_data_valid(&a->position_valid))
        { // Ok to try aircraft relative first
            reflat = a->lat;
            reflon = a->lon;
        }
        else if (lib_state.bUserFlags & MODES_USER_LATLON_VALID)
        {
            reflat = lib_state.config.latitude;
            reflon = lib_state.config.longitude;
        }
        else
        {
            // No local reference, give up
            return (-1);
        }

        result = decode_cpr_surface(reflat, reflon,
                                    a->cpr_even_lat, a->cpr_even_lon,
                                    a->cpr_odd_lat, a->cpr_odd_lon,
                                    fflag,
                                    lat, lon);
    }
    else
    {
        // airborne global CPR
        result = decode_cpr_airborne(a->cpr_even_lat, a->cpr_even_lon,
                                     a->cpr_odd_lat, a->cpr_odd_lon,
                                     fflag,
                                     lat, lon);
    }

    if (result < 0)
    {
        return result;
    }

    // check max range
    if (lib_state.config.max_range > 0 && (lib_state.bUserFlags & MODES_USER_LATLON_VALID))
    {
        double range = greatcircle(lib_state.config.latitude, lib_state.config.longitude, *lat, *lon);
        if (range > lib_state.config.max_range)
        {
            lib_state.stats_current.cpr_global_range_checks++;
            return (-2); // we consider an out-of-range value to be bad data
        }
    }

    // for mlat results, skip the speed check
    if (mm->source == SOURCE_MLAT)
        return result;

    // check speed limit
    if (track_data_valid(&a->position_valid) && mm->source <= a->position_valid.source && !speed_check(a, *lat, *lon, surface))
    {
        lib_state.stats_current.cpr_global_speed_checks++;
        return -2;
    }

    return result;
}

static int do_local_cpr(struct aircraft *a, modes_message_t *mm, double *lat, double *lon, unsigned *nic, unsigned *rc)
{
    // relative CPR
    // find reference location
    double reflat, reflon;
    double range_limit = 0;
    int result;
    int fflag = mm->cpr_odd;
    int surface = (mm->cpr_type == CPR_SURFACE);
    int relative_to = 0; // aircraft(1) or receiver(2) relative

    if (fflag)
    {
        *nic = a->cpr_odd_nic;
        *rc = a->cpr_odd_rc;
    }
    else
    {
        *nic = a->cpr_even_nic;
        *rc = a->cpr_even_rc;
    }

    if (messageNow() - a->position_valid.updated < (10 * 60 * 1000))
    {
        reflat = a->lat;
        reflon = a->lon;

        if (a->nic < *nic)
            *nic = a->nic;
        if (a->rc < *rc)
            *rc = a->rc;

        range_limit = 1852 * 100; // 100NM
        // 100 NM in the 10 minutes of position validity means 600 knots which
        // is fast but happens even for commercial airliners.
        // It's not a problem if this limitation fails every now and then.
        // A wrong relative position decode would require the aircraft to
        // travel 360-100=260 NM in the 10 minutes of position validity.
        // This is impossible for planes slower than 1560 knots/Mach 2.3 over the ground.
        // Thus this range limit combined with the 10 minutes of position
        // validity should not provide bad positions (1 cell away).

        relative_to = 1;
    }
    else if (!surface && (lib_state.bUserFlags & MODES_USER_LATLON_VALID))
    {
        reflat = lib_state.config.latitude;
        reflon = lib_state.config.longitude;

        // The cell size is at least 360NM, giving a nominal
        // max range of 180NM (half a cell).
        //
        // If the receiver range is more than half a cell
        // then we must limit this range further to avoid
        // ambiguity. (e.g. if we receive a position report
        // at 200NM distance, this may resolve to a position
        // at (200-360) = 160NM in the wrong direction)

        if (lib_state.config.max_range == 0)
        {
            return (-1); // Can't do receiver-centered checks at all
        }
        else if (lib_state.config.max_range <= 1852 * 180)
        {
            range_limit = lib_state.config.max_range;
        }
        else if (lib_state.config.max_range < 1852 * 360)
        {
            range_limit = (1852 * 360) - lib_state.config.max_range;
        }
        else
        {
            return (-1); // Can't do receiver-centered checks at all
        }
        relative_to = 2;
    }
    else
    {
        // No local reference, give up
        return (-1);
    }

    result = decode_cpr_relative(reflat, reflon,
                                 mm->cpr_lat,
                                 mm->cpr_lon,
                                 fflag, surface,
                                 lat, lon);
    if (result < 0)
    {
        return result;
    }

    // check range limit
    if (range_limit > 0)
    {
        double range = greatcircle(reflat, reflon, *lat, *lon);
        if (range > range_limit)
        {
            lib_state.stats_current.cpr_local_range_checks++;
            return (-1);
        }
    }

    // check speed limit
    if (track_data_valid(&a->position_valid) && mm->source <= a->position_valid.source && !speed_check(a, *lat, *lon, surface))
    {
        lib_state.stats_current.cpr_local_speed_checks++;
        return -1;
    }

    return relative_to;
}

static uint64_t time_between(uint64_t t1, uint64_t t2)
{
    if (t1 >= t2)
        return t1 - t2;
    else
        return t2 - t1;
}

static void update_position(struct aircraft *a, modes_message_t *mm)
{
    int location_result = -1;
    uint64_t max_elapsed;
    double new_lat = 0, new_lon = 0;
    unsigned new_nic = 0;
    unsigned new_rc = 0;
    int surface;

    surface = (mm->cpr_type == CPR_SURFACE);

    if (surface)
    {
        ++lib_state.stats_current.cpr_surface;

        // Surface: 25 seconds if >25kt or speed unknown, 50 seconds otherwise
        if (mm->gs_valid && mm->gs.selected <= 25)
            max_elapsed = 50000;
        else
            max_elapsed = 25000;
    }
    else
    {
        ++lib_state.stats_current.cpr_airborne;

        // Airborne: 10 seconds
        max_elapsed = 10000;
    }

    // If we have enough recent data, try global CPR
    if (track_data_valid(&a->cpr_odd_valid) && track_data_valid(&a->cpr_even_valid) &&
        a->cpr_odd_valid.source == a->cpr_even_valid.source &&
        a->cpr_odd_type == a->cpr_even_type &&
        time_between(a->cpr_odd_valid.updated, a->cpr_even_valid.updated) <= max_elapsed)
    {

        location_result = do_global_cpr(a, mm, &new_lat, &new_lon, &new_nic, &new_rc);

        if (location_result == -2)
        {
            // Global CPR failed because the position produced implausible results.
            // This is bad data.
            // At least one of the CPRs is bad, mark them both invalid.
            // If we are not confident in the position, invalidate it as well.

            lib_state.stats_current.cpr_global_bad++;

            a->cpr_odd_valid.source = SOURCE_INVALID;
            a->cpr_even_valid.source = SOURCE_INVALID;
            a->pos_reliable_odd--;
            a->pos_reliable_even--;

            if (a->pos_reliable_odd <= 0 || a->pos_reliable_even <= 0)
            {
                a->position_valid.source = SOURCE_INVALID;
                a->pos_reliable_odd = 0;
                a->pos_reliable_even = 0;
            }

            return;
        }
        else if (location_result == -1)
        {
            // No local reference for surface position available, or the two messages crossed a zone.
            // Nonfatal, try again later.
            lib_state.stats_current.cpr_global_skipped++;
        }
        else
        {
            if (accept_data(&a->position_valid, mm->source, mm, 1))
            {
                lib_state.stats_current.cpr_global_ok++;

                if (a->pos_reliable_odd <= 0 || a->pos_reliable_even <= 0)
                {
                    a->pos_reliable_odd = 1;
                    a->pos_reliable_even = 1;
                }
                else if (mm->cpr_odd)
                {
                    a->pos_reliable_odd = min(a->pos_reliable_odd + 1, lib_state.filter_persistence);
                }
                else
                {
                    a->pos_reliable_even = min(a->pos_reliable_even + 1, lib_state.filter_persistence);
                }

                if (track_data_valid(&a->gs_valid))
                    a->gs_last_pos = a->gs;
            }
            else
            {
                lib_state.stats_current.cpr_global_skipped++;
                location_result = -2;
            }
        }
    }

    // Otherwise try relative CPR.
    if (location_result == -1)
    {
        location_result = do_local_cpr(a, mm, &new_lat, &new_lon, &new_nic, &new_rc);

        if (location_result >= 0 && accept_data(&a->position_valid, mm->source, mm, 1))
        {
            lib_state.stats_current.cpr_local_ok++;
            mm->cpr_relative = 1;

            if (track_data_valid(&a->gs_valid))
                a->gs_last_pos = a->gs;

            if (location_result == 1)
            {
                lib_state.stats_current.cpr_local_aircraft_relative++;
            }
            if (location_result == 2)
            {
                lib_state.stats_current.cpr_local_receiver_relative++;
            }
        }
        else
        {
            lib_state.stats_current.cpr_local_skipped++;
            location_result = -1;
        }
    }

    if (location_result >= 0)
    {
        // If we sucessfully decoded, back copy the results to mm so that we can print them in list output
        mm->cpr_decoded = 1;
        mm->decoded_lat = new_lat;
        mm->decoded_lon = new_lon;
        mm->decoded_nic = new_nic;
        mm->decoded_rc = new_rc;

        // Update aircraft state
        a->lat = new_lat;
        a->lon = new_lon;
        a->nic = new_nic;
        a->rc = new_rc;

        double dip, ti, gv;
        // Update magnetic declination whenever position changes
        if (track_data_valid(&a->altitude_geom_valid))
        {
            // Altitude given in feet but required to be in kilometer above WGS84 ellipsoid.
            geomag_calc(a->alt_geom * 0.0003048, a->lat, a->lon, -1.0, &a->declination, &dip, &ti, &gv);
        }

        a->distance = false;
        if (a->pos_reliable_odd >= 1 && a->pos_reliable_even >= 1 && mm->source == SOURCE_ADSB)
        {
            a->distance = update_polar_range(new_lat, new_lon);
        }
    }
}

static unsigned compute_nic(unsigned metype, unsigned version, unsigned nic_a, unsigned nic_b, unsigned nic_c)
{
    switch (metype)
    {
    case 5:  // surface
    case 9:  // airborne
    case 20: // airborne, GNSS altitude
        return 11;

    case 6:  // surface
    case 10: // airborne
    case 21: // airborne, GNSS altitude
        return 10;

    case 7: // surface
        if (version == 2)
        {
            if (nic_a && !nic_c)
            {
                return 9;
            }
            else
            {
                return 8;
            }
        }
        else if (version == 1)
        {
            if (nic_a)
            {
                return 9;
            }
            else
            {
                return 8;
            }
        }
        else
        {
            return 8;
        }

    case 8: // surface
        if (version == 2)
        {
            if (nic_a && nic_c)
            {
                return 7;
            }
            else if (nic_a && !nic_c)
            {
                return 6;
            }
            else if (!nic_a && nic_c)
            {
                return 6;
            }
            else
            {
                return 0;
            }
        }
        else
        {
            return 0;
        }

    case 11: // airborne
        if (version == 2)
        {
            if (nic_a && nic_b)
            {
                return 9;
            }
            else
            {
                return 8;
            }
        }
        else if (version == 1)
        {
            if (nic_a)
            {
                return 9;
            }
            else
            {
                return 8;
            }
        }
        else
        {
            return 8;
        }

    case 12: // airborne
        return 7;

    case 13: // airborne
        return 6;

    case 14: // airborne
        return 5;

    case 15: // airborne
        return 4;

    case 16: // airborne
        if (nic_a && nic_b)
        {
            return 3;
        }
        else
        {
            return 2;
        }

    case 17: // airborne
        return 1;

    default:
        return 0;
    }
}

static unsigned compute_rc(unsigned metype, unsigned version, unsigned nic_a, unsigned nic_b, unsigned nic_c)
{
    switch (metype)
    {
    case 5:       // surface
    case 9:       // airborne
    case 20:      // airborne, GNSS altitude
        return 8; // 7.5m

    case 6:  // surface
    case 10: // airborne
    case 21: // airborne, GNSS altitude
        return 25;

    case 7: // surface
        if (version == 2)
        {
            if (nic_a && !nic_c)
            {
                return 75;
            }
            else
            {
                return 186; // 185.2m, 0.1NM
            }
        }
        else if (version == 1)
        {
            if (nic_a)
            {
                return 75;
            }
            else
            {
                return 186; // 185.2m, 0.1NM
            }
        }
        else
        {
            return 186; // 185.2m, 0.1NM
        }

    case 8: // surface
        if (version == 2)
        {
            if (nic_a && nic_c)
            {
                return 371; // 370.4m, 0.2NM
            }
            else if (nic_a && !nic_c)
            {
                return 556; // 555.6m, 0.3NM
            }
            else if (!nic_a && nic_c)
            {
                return 926; // 926m, 0.5NM
            }
            else
            {
                return RC_UNKNOWN;
            }
        }
        else
        {
            return RC_UNKNOWN;
        }

    case 11: // airborne
        if (version == 2)
        {
            if (nic_a && nic_b)
            {
                return 75;
            }
            else
            {
                return 186; // 370.4m, 0.2NM
            }
        }
        else if (version == 1)
        {
            if (nic_a)
            {
                return 75;
            }
            else
            {
                return 186; // 370.4m, 0.2NM
            }
        }
        else
        {
            return 186; // 370.4m, 0.2NM
        }

    case 12:        // airborne
        return 371; // 370.4m, 0.2NM

    case 13: // airborne
        if (version == 2)
        {
            if (!nic_a && nic_b)
            {
                return 556; // 555.6m, 0.3NM
            }
            else if (!nic_a && !nic_b)
            {
                return 926; // 926m, 0.5NM
            }
            else if (nic_a && nic_b)
            {
                return 1112; // 1111.2m, 0.6NM
            }
            else
            {
                return RC_UNKNOWN; // bad combination, assume worst Rc
            }
        }
        else if (version == 1)
        {
            if (nic_a)
            {
                return 1112; // 1111.2m, 0.6NM
            }
            else
            {
                return 926; // 926m, 0.5NM
            }
        }
        else
        {
            return 926; // 926m, 0.5NM
        }

    case 14:         // airborne
        return 1852; // 1.0NM

    case 15:         // airborne
        return 3704; // 2NM

    case 16: // airborne
        if (version == 2)
        {
            if (nic_a && nic_b)
            {
                return 7408; // 4NM
            }
            else
            {
                return 14816; // 8NM
            }
        }
        else if (version == 1)
        {
            if (nic_a)
            {
                return 7408; // 4NM
            }
            else
            {
                return 14816; // 8NM
            }
        }
        else
        {
            return 18520; // 10NM
        }

    case 17:          // airborne
        return 37040; // 20NM

    default:
        return RC_UNKNOWN;
    }
}

/* Map ADS-B v0 position message type to NACp value
 * returned computed NACp, or -1 if not a suitable message type
 */
static int compute_v0_nacp(modes_message_t *mm)
{
    if (mm->msgtype != 17 && mm->msgtype != 18)
    {
        return -1;
    }

    // ED-102A Table N-7
    switch (mm->metype)
    {
    case 0:
        return 0;
    case 5:
        return 11;
    case 6:
        return 10;
    case 7:
        return 8;
    case 8:
        return 0;
    case 9:
        return 11;
    case 10:
        return 10;
    case 11:
        return 8;
    case 12:
        return 7;
    case 13:
        return 6;
    case 14:
        return 5;
    case 15:
        return 4;
    case 16:
        return 1;
    case 17:
        return 1;
    case 18:
        return 0;
    case 20:
        return 11;
    case 21:
        return 10;
    case 22:
        return 0;
    default:
        return -1;
    }
}

/* Map ADS-B v0 position message type to SIL value
 * returned computed SIL, or -1 if not a suitable message type
 */
static int compute_v0_sil(modes_message_t *mm)
{
    if (mm->msgtype != 17 && mm->msgtype != 18)
    {
        return -1;
    }

    // ED-102A Table N-8
    switch (mm->metype)
    {
    case 0:
        return 0;

    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
        return 2;

    case 18:
        return 0;

    case 20:
    case 21:
        return 2;

    case 22:
        return 0;

    default:
        return -1;
    }
}

static void compute_nic_rc_from_message(modes_message_t *mm, struct aircraft *a, unsigned *nic, unsigned *rc)
{
    int nic_a = (track_data_valid(&a->nic_a_valid) && a->nic_a);
    int nic_b = (mm->accuracy.nic_b_valid && mm->accuracy.nic_b);
    int nic_c = (track_data_valid(&a->nic_c_valid) && a->nic_c);

    *nic = compute_nic(mm->metype, a->adsb_version, nic_a, nic_b, nic_c);
    *rc = compute_rc(mm->metype, a->adsb_version, nic_a, nic_b, nic_c);
}

static int altitude_to_feet(int raw, altitude_unit_t unit)
{
    switch (unit)
    {
    case UNIT_METERS:
        return raw / 0.3048;
    case UNIT_FEET:
        return raw;
    default:
        return 0;
    }
}

/* Receive new messages and update tracked aircraft state
 */
struct aircraft *track_update_from_message(modes_message_t *mm)
{
    struct aircraft *a;
    unsigned int cpr_new = 0;

    if (mm->msgtype == 32)
    {
        // Mode A/C, just count it (we ignore SPI)
        modeAC_count[mode_a_to_index(mm->squawk)]++;
        return NULL;
    }

    if (mm->addr == 0)
    {
        // junk address, don't track it
        return NULL;
    }

    _messageNow = mm->sysTimestampMsg;

    // Lookup our aircraft or create a new one
    a = track_find_aircraft(mm->addr);
    if (!a)
    {                                                                // If it's a currently unknown aircraft....
        a = track_create_aircraft(mm);                               // ., create a new record for it,
        a->next = lib_state.aircrafts[mm->addr % AIRCRAFTS_BUCKETS]; // .. and put it at the head of the list
        lib_state.aircrafts[mm->addr % AIRCRAFTS_BUCKETS] = a;
    }

    if (mm->signalLevel > 0)
    {
        a->signalLevel[a->signalNext] = mm->signalLevel;
        a->signalNext = (a->signalNext + 1) & 7;
    }
    a->seen_ms = mm->sysTimestampMsg;
    a->messages++;

    // update addrtype, we only ever go towards "more direct" types
    if (mm->addrtype < a->addr_type)
    {
        a->addr_type = mm->addrtype;
    }

    // decide on where to stash the version
    int dummy_version = -1; // used for non-adsb/adsr/tisb messages
    int *message_version;

    switch (mm->source)
    {
    case SOURCE_ADSB:
        message_version = &a->adsb_version;
        break;
    case SOURCE_TISB:
        message_version = &a->tisb_version;
        break;
    case SOURCE_ADSR:
        message_version = &a->adsr_version;
        break;
    default:
        message_version = &dummy_version;
        break;
    }

    // assume version 0 until we see something else
    if (*message_version < 0)
    {
        *message_version = 0;
    }

    // category shouldn't change over time, don't bother with metadata
    if (mm->category_valid)
    {
        a->category = mm->category;
    }

    // operational status message
    // done early to update version / HRD / TAH
    if (mm->opstatus.valid)
    {
        *message_version = mm->opstatus.version;

        if (mm->opstatus.hrd != HEADING_INVALID)
        {
            a->adsb_hrd = mm->opstatus.hrd;
        }
        if (mm->opstatus.tah != HEADING_INVALID)
        {
            a->adsb_tah = mm->opstatus.tah;
        }
    }

    // fill in ADS-B v0 NACp, SIL from position message type
    if (*message_version == 0 && !mm->accuracy.nac_p_valid)
    {
        int computed_nacp = compute_v0_nacp(mm);
        if (computed_nacp != -1)
        {
            mm->accuracy.nac_p_valid = 1;
            mm->accuracy.nac_p = computed_nacp;
        }
    }

    if (*message_version == 0 && mm->accuracy.sil_type == SIL_INVALID)
    {
        int computed_sil = compute_v0_sil(mm);
        if (computed_sil != -1)
        {
            mm->accuracy.sil_type = SIL_UNKNOWN;
            mm->accuracy.sil = computed_sil;
        }
    }

    if (mm->altitude_baro_valid &&
        (mm->source >= a->altitude_baro_valid.source ||
         track_data_age(&a->altitude_baro_valid) > 15 * 1000))
    {
        int alt = altitude_to_feet(mm->altitude_baro, mm->altitude_baro_unit);
        if (a->modeC_hit)
        {
            int new_modeC = (a->alt_baro + 49) / 100;
            int old_modeC = (alt + 49) / 100;
            if (new_modeC != old_modeC)
            {
                a->modeC_hit = 0;
            }
        }

        int delta = alt - a->alt_baro;
        int fpm = 0;

        int max_fpm = 12500;
        int min_fpm = -12500;

        if (abs(delta) >= 300)
        {
            fpm = delta * 60 * 10 / (abs((int)track_data_age(&a->altitude_baro_valid) / 100) + 10);
            if (track_data_valid(&a->geom_rate_valid) && track_data_age(&a->geom_rate_valid) < track_data_age(&a->baro_rate_valid))
            {
                min_fpm = a->geom_rate - 1500 - min(11000, ((int)track_data_age(&a->geom_rate_valid) / 2));
                max_fpm = a->geom_rate + 1500 + min(11000, ((int)track_data_age(&a->geom_rate_valid) / 2));
            }
            else if (track_data_valid(&a->baro_rate_valid))
            {
                min_fpm = a->baro_rate - 1500 - min(11000, ((int)track_data_age(&a->baro_rate_valid) / 2));
                max_fpm = a->baro_rate + 1500 + min(11000, ((int)track_data_age(&a->baro_rate_valid) / 2));
            }
            if (track_data_valid(&a->altitude_baro_valid) && track_data_age(&a->altitude_baro_valid) < 30000)
            {
                a->altitude_baro_reliable = min(
                    ALTITUDE_BARO_RELIABLE_MAX - (ALTITUDE_BARO_RELIABLE_MAX * track_data_age(&a->altitude_baro_valid) / 30000),
                    a->altitude_baro_reliable);
            }
            else
            {
                a->altitude_baro_reliable = 0;
            }
        }
        int good_crc = (mm->crc == 0 && mm->source != SOURCE_MLAT) ? (ALTITUDE_BARO_RELIABLE_MAX / 2 - 1) : 0;

        if (a->altitude_baro_reliable <= 0 || abs(delta) < 300 || (fpm < max_fpm && fpm > min_fpm) || (good_crc && a->altitude_baro_reliable <= (ALTITUDE_BARO_RELIABLE_MAX / 2 + 2)))
        {
            if (accept_data(&a->altitude_baro_valid, mm->source, mm, 1))
            {
                a->altitude_baro_reliable = min(ALTITUDE_BARO_RELIABLE_MAX, a->altitude_baro_reliable + (good_crc + 1));
                /*if (abs(delta) > 2000 && delta != alt) {
                    fprintf(stderr, "Alt change B: %06x: %d   %d -> %d, min %.1f kfpm, max %.1f kfpm, actual %.1f kfpm\n",
                        a->addr, a->altitude_baro_reliable, a->altitude_baro, alt, min_fpm/1000.0, max_fpm/1000.0, fpm/1000.0);
                }*/
                a->alt_baro = alt;
            }
        }
        else
        {
            a->altitude_baro_reliable = a->altitude_baro_reliable - (good_crc + 1);
            //fprintf(stderr, "Alt check F: %06x: %d   %d -> %d, min %.1f kfpm, max %.1f kfpm, actual %.1f kfpm\n",
            //        a->addr, a->altitude_baro_reliable, a->altitude_baro, alt, min_fpm/1000.0, max_fpm/1000.0, fpm/1000.0);
            if (a->altitude_baro_reliable <= 0)
            {
                //fprintf(stderr, "Altitude INVALIDATED: %06x\n", a->addr);
                a->altitude_baro_reliable = 0;
                a->altitude_baro_valid.source = SOURCE_INVALID;
            }
        }
    }

    if (mm->squawk_valid && accept_data(&a->squawk_valid, mm->source, mm, 0))
    {
        if (mm->squawk != a->squawk)
        {
            a->modeA_hit = 0;
        }
        a->squawk = mm->squawk;

        // Disabled for now as it obscures the origin of the data
        // Handle 7x00 without a corresponding emergency status
#if 0
        if (!mm->emergency_valid) {
            emergency_t squawk_emergency;
            switch (mm->squawk) {
                case 0x7500:
                    squawk_emergency = EMERGENCY_UNLAWFUL;
                    break;
                case 0x7600:
                    squawk_emergency = EMERGENCY_NORDO;
                    break;
                case 0x7700:
                    squawk_emergency = EMERGENCY_GENERAL;
                    break;
                default:
                    squawk_emergency = EMERGENCY_NONE;
                    break;
            }

            if (squawk_emergency != EMERGENCY_NONE && accept_data(&a->emergency_valid, mm->source, mm, 0)) {
                a->emergency = squawk_emergency;
            }
        }
#endif
    }

    if (mm->emergency_valid && accept_data(&a->emergency_valid, mm->source, mm, 0))
    {
        a->emergency = mm->emergency;
    }

    if (mm->altitude_geom_valid && accept_data(&a->altitude_geom_valid, mm->source, mm, 1))
    {
        a->alt_geom = altitude_to_feet(mm->altitude_geom, mm->altitude_geom_unit);
    }

    if (mm->geom_delta_valid && accept_data(&a->geom_delta_valid, mm->source, mm, 1))
    {
        a->geom_delta = mm->geom_delta;
    }

    if (mm->heading_valid)
    {
        a->heading_type = mm->heading_type;
        if (a->heading_type == HEADING_MAGNETIC_OR_TRUE)
        {
            a->heading_type = a->adsb_hrd;
        }
        else if (a->heading_type == HEADING_TRACK_OR_HEADING)
        {
            a->heading_type = a->adsb_tah;
        }

        if (a->heading_type == HEADING_GROUND_TRACK && accept_data(&a->track_valid, mm->source, mm, 1))
        {
            a->track = mm->heading;
        }
        else if (a->heading_type == HEADING_MAGNETIC && accept_data(&a->mag_heading_valid, mm->source, mm, 1))
        {
            a->mag_heading = mm->heading;
        }
        else if (a->heading_type == HEADING_TRUE && accept_data(&a->true_heading_valid, mm->source, mm, 1))
        {
            a->true_heading = mm->heading;
        }
    }

    if (mm->track_rate_valid && accept_data(&a->track_rate_valid, mm->source, mm, 1))
    {
        a->track_rate = mm->track_rate;
    }

    if (mm->roll_valid && accept_data(&a->roll_valid, mm->source, mm, 1))
    {
        a->roll = mm->roll;
    }

    if (mm->gs_valid)
    {
        mm->gs.selected = (*message_version == 2 ? mm->gs.v2 : mm->gs.v0);
        if (accept_data(&a->gs_valid, mm->source, mm, 1))
        {
            a->gs = mm->gs.selected;
        }
    }

    if (mm->ias_valid && accept_data(&a->ias_valid, mm->source, mm, 0))
    {
        a->ias = mm->ias;
    }

    if (mm->tas_valid && accept_data(&a->tas_valid, mm->source, mm, 0))
    {
        a->tas = mm->tas;
    }

    if (mm->mach_valid && accept_data(&a->mach_valid, mm->source, mm, 0))
    {
        a->mach = mm->mach;
    }

    if (mm->baro_rate_valid && accept_data(&a->baro_rate_valid, mm->source, mm, 1))
    {
        a->baro_rate = mm->baro_rate;
    }

    if (mm->geom_rate_valid && accept_data(&a->geom_rate_valid, mm->source, mm, 1))
    {
        a->geom_rate = mm->geom_rate;
    }

    if (mm->airground != AG_INVALID)
    {
        // If our current state is UNCERTAIN, accept new data as normal
        // If our current state is certain but new data is not, only accept the uncertain state if the certain data has gone stale
        if (mm->airground != AG_UNCERTAIN ||
            (mm->airground == AG_UNCERTAIN && !track_data_stale(&a->airground_valid)))
        {
            if (accept_data(&a->airground_valid, mm->source, mm, 0))
            {
                a->air_ground = mm->airground;
            }
        }
    }

    if (mm->callsign_valid && accept_data(&a->callsign_valid, mm->source, mm, 0))
    {
        memcpy(a->flight_id, mm->callsign, sizeof(a->flight_id));
    }

    if (mm->nav.mcp_altitude_valid && accept_data(&a->nav_altitude_mcp_valid, mm->source, mm, 0))
    {
        a->nav_altitude_mcp = mm->nav.mcp_altitude;
    }

    if (mm->nav.fms_altitude_valid && accept_data(&a->nav_altitude_fms_valid, mm->source, mm, 0))
    {
        a->nav_altitude_fms = mm->nav.fms_altitude;
    }

    if (mm->nav.altitude_source != NAV_ALT_INVALID && accept_data(&a->nav_altitude_src_valid, mm->source, mm, 0))
    {
        a->nav_altitude_src = mm->nav.altitude_source;
    }

    if (mm->nav.heading_valid && accept_data(&a->nav_heading_valid, mm->source, mm, 0))
    {
        a->nav_heading = mm->nav.heading;
    }

    if (mm->nav.modes_valid && accept_data(&a->nav_modes_valid, mm->source, mm, 0))
    {
        if (mm->nav.modes & NAV_MODE_AUTOPILOT)
        {
            a->nav_modes.autopilot = true;
        }
        if (mm->nav.modes & NAV_MODE_VNAV)
        {
            a->nav_modes.vnav = true;
        }
        if (mm->nav.modes & NAV_MODE_ALT_HOLD)
        {
            a->nav_modes.althold = true;
        }
        if (mm->nav.modes & NAV_MODE_APPROACH)
        {
            a->nav_modes.approach = true;
        }
        if (mm->nav.modes & NAV_MODE_LNAV)
        {
            a->nav_modes.lnav = true;
        }
        if (mm->nav.modes & NAV_MODE_TCAS)
        {
            a->nav_modes.tcas = true;
        }
    }

    if (mm->nav.qnh_valid && accept_data(&a->nav_qnh_valid, mm->source, mm, 0))
    {
        a->nav_qnh = mm->nav.qnh;
    }

    if (mm->alert_valid && accept_data(&a->alert_valid, mm->source, mm, 0))
    {
        a->alert = mm->alert;
    }

    if (mm->spi_valid && accept_data(&a->spi_valid, mm->source, mm, 0))
    {
        a->spi = mm->spi;
    }

    // CPR, even
    if (mm->cpr_valid && !mm->cpr_odd && accept_data(&a->cpr_even_valid, mm->source, mm, 1))
    {
        a->cpr_even_type = mm->cpr_type;
        a->cpr_even_lat = mm->cpr_lat;
        a->cpr_even_lon = mm->cpr_lon;
        compute_nic_rc_from_message(mm, a, &a->cpr_even_nic, &a->cpr_even_rc);
        cpr_new = 1;
    }

    // CPR, odd
    if (mm->cpr_valid && mm->cpr_odd && accept_data(&a->cpr_odd_valid, mm->source, mm, 1))
    {
        a->cpr_odd_type = mm->cpr_type;
        a->cpr_odd_lat = mm->cpr_lat;
        a->cpr_odd_lon = mm->cpr_lon;
        compute_nic_rc_from_message(mm, a, &a->cpr_odd_nic, &a->cpr_odd_rc);
        cpr_new = 1;
    }

    if (mm->accuracy.sda_valid && accept_data(&a->sda_valid, mm->source, mm, 0))
    {
        a->sda = mm->accuracy.sda;
    }

    if (mm->accuracy.nic_a_valid && accept_data(&a->nic_a_valid, mm->source, mm, 0))
    {
        a->nic_a = mm->accuracy.nic_a;
    }

    if (mm->accuracy.nic_c_valid && accept_data(&a->nic_c_valid, mm->source, mm, 0))
    {
        a->nic_c = mm->accuracy.nic_c;
    }

    if (mm->accuracy.nic_baro_valid && accept_data(&a->nic_baro_valid, mm->source, mm, 0))
    {
        a->nic_baro = mm->accuracy.nic_baro;
    }

    if (mm->accuracy.nac_p_valid && accept_data(&a->nac_p_valid, mm->source, mm, 0))
    {
        a->nac_p = mm->accuracy.nac_p;
    }

    if (mm->accuracy.nac_v_valid && accept_data(&a->nac_v_valid, mm->source, mm, 0))
    {
        a->nac_v = mm->accuracy.nac_v;
    }

    if (mm->accuracy.sil_type != SIL_INVALID && accept_data(&a->sil_valid, mm->source, mm, 0))
    {
        a->sil = mm->accuracy.sil;
        if (a->sil_type == SIL_INVALID || mm->accuracy.sil_type != SIL_UNKNOWN)
        {
            a->sil_type = mm->accuracy.sil_type;
        }
    }

    if (mm->accuracy.gva_valid && accept_data(&a->gva_valid, mm->source, mm, 0))
    {
        a->gva = mm->accuracy.gva;
    }

    if (mm->accuracy.sda_valid && accept_data(&a->sda_valid, mm->source, mm, 0))
    {
        a->sda = mm->accuracy.sda;
    }

    // Now handle derived data

    // derive geometric altitude if we have baro + delta
    if (a->altitude_baro_reliable >= 3 && compare_validity(&a->altitude_baro_valid, &a->altitude_geom_valid) > 0 &&
        compare_validity(&a->geom_delta_valid, &a->altitude_geom_valid) > 0)
    {
        // Baro and delta are both more recent than geometric, derive geometric from baro + delta
        a->alt_geom = a->alt_baro + a->geom_delta;
        combine_validity(&a->altitude_geom_valid, &a->altitude_baro_valid, &a->geom_delta_valid);
    }

    // If we've got a new cpr_odd or cpr_even
    if (cpr_new)
    {
        update_position(a, mm);
    }

    if (mm->sbs_in && mm->decoded_lat != 0 && mm->decoded_lon != 0)
    {
        if (accept_data(&a->position_valid, mm->source, mm, 0))
        {
            a->lat = mm->decoded_lat;
            a->lon = mm->decoded_lon;

            a->pos_reliable_odd = 2;
            a->pos_reliable_even = 2;
        }
    }

    if (mm->msgtype == 11 && mm->IID == 0 && mm->correctedbits == 0 && messageNow() > a->next_reduce_forward_DF11)
    {

        a->next_reduce_forward_DF11 = messageNow();
        mm->reduce_forward = 1;
    }

    return (a);
}

/* Periodic updates of tracking state
 * Periodically match up mode A/C results with mode S results
 */
static void track_match_ac(uint64_t now)
{
    // clear match flags
    for (unsigned i = 0; i < 4096; ++i)
    {
        modeAC_match[i] = 0;
    }

    // scan aircraft list, look for matches
    for (int j = 0; j < AIRCRAFTS_BUCKETS; j++)
    {
        for (struct aircraft *a = lib_state.aircrafts[j]; a; a = a->next)
        {
            if ((now - a->seen_ms) > 5000)
            {
                continue;
            }

            // match on Mode A
            if (track_data_valid(&a->squawk_valid))
            {
                unsigned i = mode_a_to_index(a->squawk);
                if ((modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES)
                {
                    a->modeA_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }
            }

            // match on Mode C (+/- 100ft)
            if (track_data_valid(&a->altitude_baro_valid))
            {
                int modeC = (a->alt_baro + 49) / 100;

                unsigned modeA = mode_c_to_mode_a(modeC);
                unsigned i = mode_a_to_index(modeA);
                if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES)
                {
                    a->modeC_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }

                modeA = mode_c_to_mode_a(modeC + 1);
                i = mode_a_to_index(modeA);
                if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES)
                {
                    a->modeC_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }

                modeA = mode_c_to_mode_a(modeC - 1);
                i = mode_a_to_index(modeA);
                if (modeA && (modeAC_count[i] - modeAC_lastcount[i]) >= TRACK_MODEAC_MIN_MESSAGES)
                {
                    a->modeC_hit = 1;
                    modeAC_match[i] = (modeAC_match[i] ? 0xFFFFFFFF : a->addr);
                }
            }
        }
    }

    // reset counts for next time
    for (unsigned i = 0; i < 4096; ++i)
    {
        if (!modeAC_count[i])
            continue;

        if ((modeAC_count[i] - modeAC_lastcount[i]) < TRACK_MODEAC_MIN_MESSAGES)
        {
            if (++modeAC_age[i] > 15)
            {
                // not heard from for a while, clear it out
                modeAC_lastcount[i] = modeAC_count[i] = modeAC_age[i] = 0;
            }
        }
        else
        {
            // this one is live
            // set a high initial age for matches, so they age out rapidly
            // and don't show up on the interactive display when the matching
            // mode S data goes away or changes
            if (modeAC_match[i])
            {
                modeAC_age[i] = 10;
            }
            else
            {
                modeAC_age[i] = 0;
            }
        }

        modeAC_lastcount[i] = modeAC_count[i];
    }
}

/* If we don't receive new nessages within TRACK_AIRCRAFT_TTL
 * we remove the aircraft from the list.
 */
static void track_remove_stale_aircraft(uint64_t now)
{
    for (int j = 0; j < AIRCRAFTS_BUCKETS; j++)
    {
        struct aircraft *a = lib_state.aircrafts[j];
        struct aircraft *prev = NULL;

        while (a)
        {
            if ((now - a->seen_ms) > TRACK_AIRCRAFT_TTL ||
                (a->messages == 1 && (now - a->seen_ms) > TRACK_AIRCRAFT_ONEHIT_TTL))
            {
                // Count aircraft where we saw only one message before reaping them.
                // These are likely to be due to messages with bad addresses.
                if (a->messages == 1)
                    lib_state.stats_current.single_message_aircraft++;

                // Remove the element from the linked list, with care
                // if we are removing the first element
                if (!prev)
                {
                    lib_state.aircrafts[j] = a->next;
                    free(a);
                    a = lib_state.aircrafts[j];
                }
                else
                {
                    prev->next = a->next;
                    free(a);
                    a = prev->next;
                }
            }
            else
            {

#define EXPIRE(_f)                                                                  \
    do                                                                              \
    {                                                                               \
        if (a->_f##_valid.source != SOURCE_INVALID && now >= a->_f##_valid.expires) \
        {                                                                           \
            a->_f##_valid.source = SOURCE_INVALID;                                  \
        }                                                                           \
    } while (0)
                EXPIRE(callsign);
                EXPIRE(altitude_baro);
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
                EXPIRE(airground);
                EXPIRE(nav_qnh);
                EXPIRE(nav_altitude_mcp);
                EXPIRE(nav_altitude_fms);
                EXPIRE(nav_altitude_src);
                EXPIRE(nav_heading);
                EXPIRE(nav_modes);
                EXPIRE(cpr_odd);
                EXPIRE(cpr_even);
                EXPIRE(position);
                EXPIRE(nic_a);
                EXPIRE(nic_c);
                EXPIRE(nic_baro);
                EXPIRE(nac_p);
                EXPIRE(sil);
                EXPIRE(gva);
                EXPIRE(sda);
#undef EXPIRE

                // reset position reliability when the position has expired
                if (a->position_valid.source == SOURCE_INVALID)
                {
                    a->pos_reliable_odd = 0;
                    a->pos_reliable_even = 0;
                }

                if (a->altitude_baro_valid.source == SOURCE_INVALID)
                    a->altitude_baro_reliable = 0;

                prev = a;
                a = a->next;
            }
        }
    }
}

//
// Entry point for periodic updates
//

void track_periodic_update()
{
    static uint64_t next_update;
    uint64_t now = mstime();

    // Only do updates once per second
    if (now >= next_update)
    {
        next_update = now + 1000;
        track_remove_stale_aircraft(now);
        if (lib_state.config.mode_ac)
        {
            track_match_ac(now);
        }
    }
}
