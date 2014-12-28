/*

    Read .ttbin files from TomTom GPS watches

    Copyright (C) 2014 Dan Callaghan <djc@djc.id.au>
    Copyright (C) 2001-2014 Robert Lipe, robertlipe+source@gpsbabel.org

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
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111 USA

 */

/*
    This format is not publicly documented, it has been reverse-engineered.
    Initial reverse engineering done by FluffyKaon:
    https://github.com/FluffyKaon/TomTom-ttbin-file-format
*/

#include <QtCore/QHash>
#include "defs.h"

#define MYNAME "ttbin"

static gbfile *file_in;
static QHash<uint8_t, uint16_t> record_lengths;
static route_head *current_track = NULL;
static Waypoint *current_wpt = NULL;

static inline uint8_t
ttbin_getuint8(gbfile *f)
{
  int c = gbfgetc(f);
  is_fatal(c < 0, MYNAME ": Unexpected end of file");
  return (uint8_t) c;
}

/* Tag 0x20 */
static void
ttbin_read_header(void)
{
  // file format version
  uint8_t file_format = ttbin_getuint8(file_in);
  is_fatal(file_format != 7,
      MYNAME ": Unrecognized format version %d (expected 7)", file_format);

  // watch version
  uint8_t watch_version[4];
  is_fatal((gbfread(&watch_version, 1, sizeof(watch_version), file_in) != sizeof(watch_version)),
      MYNAME ": Unexpected end of file");

  // unknown 2 bytes (eb 03)
  (void) gbfgetuint16(file_in);

  // timestamp
  uint32_t timestamp = gbfgetuint32(file_in);

  // unknown variable length, mostly zeroes
  uint8_t unknown;
  do {
    unknown = ttbin_getuint8(file_in);
  } while (unknown == 0x00 || unknown == 0x2d);
  gbfungetc(unknown, file_in);

  // timestamp again
  uint32_t timestamp_again = gbfgetuint32(file_in);
  if (timestamp_again != timestamp) {
    warning(MYNAME ": Second header timestamp (%d) did not match first (%d)\n",
        timestamp_again, timestamp);
  }

  // unknown 5 bytes (a0 8c 00 00 00)
  (void) gbfgetuint32(file_in);
  (void) ttbin_getuint8(file_in);

  // record length dictionary
  uint8_t record_type_count = ttbin_getuint8(file_in);
  for (int i = 0; i < record_type_count; i ++) {
    uint8_t tag = ttbin_getuint8(file_in);
    uint16_t length = gbfgetuint16(file_in);
    // length includes the initial tag byte
    record_lengths[tag] = length - 1;
  }
}

/* Tag 0x21 */
static void
ttbin_read_segment(void)
{
  // indicator type
  // 0 => workout start
  // 1 => segment start
  // 2 => segment end
  // 3 => workout end
  uint8_t indicator = ttbin_getuint8(file_in);

  // activity type
  // 0 => running
  // 1 => cycling
  uint8_t activity = ttbin_getuint8(file_in);

  // timestamp
  uint32_t timestamp = gbfgetuint32(file_in);

  switch (indicator) {
    case 0:
    case 3:
      // workout start/end are not useful, we ignore them
      return;
    case 1:
      // start new segment
      current_track = route_head_alloc();
      switch (activity) {
        case 0:
          current_track->rte_name = QString("Running");
          break;
        case 1:
          current_track->rte_name = QString("Cycling");
          break;
        default:
          current_track->rte_name = QString("Activity %1").arg(activity);
      }
      track_add_head(current_track);
      break;
    case 2:
      current_track = NULL;
      break;
    default:
      warning(MYNAME ": Ignoring unrecognised segment indicator 0x%02x\n", indicator);
      return;
  }
  current_wpt = NULL;
}

/* Tag 0x22 */
static void
ttbin_read_gps(void)
{
  is_fatal(current_track == NULL, MYNAME ": Found GPS record outside of segment");

  // lat/long in 1e-7 degrees
  int32_t latitude = gbfgetint32(file_in);
  int32_t longitude = gbfgetint32(file_in);

  // unknown
  (void) gbfgetuint16(file_in);

  // speed in 0.01 m/s
  uint16_t speed = gbfgetuint16(file_in);

  // timestamp
  uint32_t timestamp = gbfgetuint32(file_in);

  // calories
  uint16_t calories = gbfgetuint16(file_in);

  // distance since last point in m
  float inc_distance = gbfgetflt(file_in);

  // cumulative distance in m
  float cum_distance = gbfgetflt(file_in);

  // "cycles", probably steps
  uint8_t cycles = ttbin_getuint8(file_in);

  if (timestamp == 0xffffffff) {
    // no GPS fix
    return;
  }
  Waypoint *wpt = new Waypoint();
  wpt->latitude = (double) latitude * 1e-7;
  wpt->longitude = (double) longitude * 1e-7;
  WAYPT_SET(wpt, speed, (float) speed * 0.01);
  wpt->SetCreationTime((time_t) timestamp);
  track_add_wpt(current_track, wpt);
  current_wpt = wpt;
}

/* Tag 0x25 */
static void
ttbin_read_heartrate(void)
{
  // heart rate
  uint8_t heart_rate = ttbin_getuint8(file_in);

  // unknown
  (void) ttbin_getuint8(file_in);

  // timestamp
  uint32_t timestamp = gbfgetuint32(file_in);

  if (current_wpt == NULL) {
    warning(MYNAME ": Ignoring heartrate before GPS record\n");
  } else {
    current_wpt->heartrate = heart_rate;
  }
}

/* Tag 0x2f */
static void
ttbin_read_lap(void)
{
  // time since start of lap in seconds
  uint32_t duration = gbfgetuint32(file_in);

  // cumulative distance at the end of this lap in m
  float distance = gbfgetflt(file_in);

  // unknown
  (void) gbfgetuint16(file_in);
}

static void
ttbin_skip_unknown(uint8_t tag)
{
  if (!record_lengths.contains(tag)) {
    warning(MYNAME ": Tag 0x%02x with unknown length\n", tag);
    return;
  }
  uint16_t length = record_lengths[tag];
  is_fatal(gbfseek(file_in, length, SEEK_CUR),
      MYNAME ": Unexpected end of file while skipping unknown tag 0x%02x", tag);
}

static
arglist_t ttbin_args[] = {
  ARG_TERMINATOR
};

static void
ttbin_rd_init(const char *fname)
{
  file_in = gbfopen_le(fname, "rb", MYNAME);
}

static void
ttbin_rd_deinit(void)
{
  gbfclose(file_in);
}

static void
ttbin_read(void)
{
  uint8_t tag;

  tag = ttbin_getuint8(file_in);
  is_fatal(tag != 0x20,
      MYNAME ": Expected header tag 0x20 at first byte, "
      "found 0x%02x (is this a .ttbin file?)", tag);
  ttbin_read_header();

  while (!gbfeof(file_in)) {
    tag = ttbin_getuint8(file_in);
    switch (tag) {
      case 0x21:
        ttbin_read_segment();
        break;
      case 0x22:
        ttbin_read_gps();
        break;
      case 0x25:
        ttbin_read_heartrate();
        break;
      case 0x2f:
        ttbin_read_lap();
        break;
      case 0x27:
        //ttbin_read_summary();
        //break;
      case 0x32:
        //ttbin_read_treadmill();
        //break;
      case 0x34:
        //ttbin_read_swim();
        //break;
      case 0x23:
      case 0x2a:
      case 0x2b:
      case 0x2d:
      case 0x30:
      case 0x31:
      case 0x35:
      case 0x37:
      case 0x39:
      case 0x3a:
      case 0x3b:
      case 0x3c:
      case 0x3d:
      case 0x3e:
      case 0x3f:
      default:
        ttbin_skip_unknown(tag);
    }
  }
}

ff_vecs_t ttbin_vecs = {
  ff_type_file,
  {
    ff_cap_read /* waypoints */,
    ff_cap_none /* tracks */,
    ff_cap_none /* routes */
  },
  ttbin_rd_init,
  NULL,
  ttbin_rd_deinit,
  NULL,
  ttbin_read,
  NULL,
  NULL,
  ttbin_args,
  CET_CHARSET_ASCII, 0
};
