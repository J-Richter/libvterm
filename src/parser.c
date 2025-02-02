#include "vterm_internal.h"

#include <stdio.h>
#include <string.h>

#undef DEBUG_PARSER

static bool is_intermed(unsigned char c)
{
  return c >= 0x20 && c <= 0x2f;
}

static void do_control(VTerm *vt, unsigned char control)
{
  if(vt->parser.callbacks && vt->parser.callbacks->control)
    if((*vt->parser.callbacks->control)(control, vt->parser.cbdata))
      return;

  DEBUG_LOG("libvterm: Unhandled control 0x%02x\n", control);
}

static void do_csi(VTerm *vt, char command)
{
#ifdef DEBUG_PARSER
  printf("Parsed CSI args as:\n", arglen, args);
  printf(" leader: %s\n", vt->parser.v.csi.leader);
  for(int argi = 0; argi < vt->parser.v.csi.argi; argi++) {
    printf(" %lu", CSI_ARG(vt->parser.v.csi.args[argi]));
    if(!CSI_ARG_HAS_MORE(vt->parser.v.csi.args[argi]))
      printf("\n");
  printf(" intermed: %s\n", vt->parser.intermed);
  }
#endif

  if(vt->parser.callbacks && vt->parser.callbacks->csi)
    if((*vt->parser.callbacks->csi)(
          vt->parser.v.csi.leaderlen ? vt->parser.v.csi.leader : NULL, 
          vt->parser.v.csi.args,
          vt->parser.v.csi.argi,
          vt->parser.intermedlen ? vt->parser.intermed : NULL,
          command,
          vt->parser.cbdata))
      return;

  DEBUG_LOG("libvterm: Unhandled CSI %c\n", command);
}

static void do_escape(VTerm *vt, char command)
{
  char seq[INTERMED_MAX+1];

  size_t len = vt->parser.intermedlen;
  strncpy(seq, vt->parser.intermed, len);
  seq[len++] = command;
  seq[len]   = 0;

  if(vt->parser.callbacks && vt->parser.callbacks->escape)
    if((*vt->parser.callbacks->escape)(seq, len, vt->parser.cbdata))
      return;

  DEBUG_LOG("libvterm: Unhandled escape ESC 0x%02x\n", command);
}

static void string_fragment(VTerm *vt, const char *str, size_t len, bool final)
{
  VTermStringFragment frag = {
    .str     = str,
    .len     = len,
    .initial = vt->parser.string_initial,
    .final   = final,
  };

  switch(vt->parser.state) {
    case OSC:
      if(vt->parser.callbacks && vt->parser.callbacks->osc)
        (*vt->parser.callbacks->osc)(vt->parser.v.osc.command, frag, vt->parser.cbdata);
      break;

    case DCS:
      if(len && vt->parser.callbacks && vt->parser.callbacks->dcs)
        (*vt->parser.callbacks->dcs)(vt->parser.v.dcs.command, vt->parser.v.dcs.commandlen, frag, vt->parser.cbdata);
      break;

    case NORMAL:
    case CSI_LEADER:
    case CSI_ARGS:
    case CSI_INTERMED:
    case OSC_COMMAND:
    case DCS_COMMAND:
      break;
  }

  vt->parser.string_initial = false;
}

size_t vterm_input_write(VTerm *vt, const char *bytes, size_t len)
{
  size_t pos = 0;
  const char *string_start;

  switch(vt->parser.state) {
  case NORMAL:
  case CSI_LEADER:
  case CSI_ARGS:
  case CSI_INTERMED:
  case OSC_COMMAND:
  case DCS_COMMAND:
    string_start = NULL;
    break;
  case OSC:
  case DCS:
    string_start = bytes;
    break;
  }

#define ENTER_STATE(st)        do { vt->parser.state = st; string_start = NULL; } while(0)
#define ENTER_NORMAL_STATE()   ENTER_STATE(NORMAL)

  for( ; pos < len; pos++) {
    unsigned char c = bytes[pos];
    bool c1_allowed = !vt->mode.utf8;

    if(c == 0x00 || c == 0x7f) { // NUL, DEL
      if(vt->parser.state >= OSC) {
        string_fragment(vt, string_start, bytes + pos - string_start, false);
        string_start = bytes + pos + 1;
      }
      continue;
    }
    if(c == 0x18 || c == 0x1a) { // CAN, SUB
      vt->parser.in_esc = false;
      ENTER_NORMAL_STATE();
      continue;
    }
    else if(c == 0x1b) { // ESC
      vt->parser.intermedlen = 0;
      if(vt->parser.state < OSC)
        vt->parser.state = NORMAL;
      vt->parser.in_esc = true;
      continue;
    }
    else if(c == 0x07 &&  // BEL, can stand for ST in OSC or DCS state
            vt->parser.state >= OSC) {
      // fallthrough
    }
    else if(c < 0x20) { // other C0
      if(vt->parser.state >= OSC)
        string_fragment(vt, string_start, bytes + pos - string_start, false);
      do_control(vt, c);
      if(vt->parser.state >= OSC)
        string_start = bytes + pos + 1;
      continue;
    }
    // else fallthrough

    size_t string_len = bytes + pos - string_start;

    if(vt->parser.in_esc) {
      // Hoist an ESC letter into a C1 if we're not in a string mode
      // Always accept ESC \ == ST even in string mode
      if(!vt->parser.intermedlen &&
          c >= 0x40 && c < 0x60 &&
          ((vt->parser.state < OSC || c == 0x5c))) {
        c += 0x40;
        c1_allowed = true;
        string_len -= 1;
        vt->parser.in_esc = false;
      }
      else {
        string_start = NULL;
        vt->parser.state = NORMAL;
      }
    }

    switch(vt->parser.state) {
    case CSI_LEADER:
      /* Extract leader bytes 0x3c to 0x3f */
      if(c >= 0x3c && c <= 0x3f) {
        if(vt->parser.v.csi.leaderlen < CSI_LEADER_MAX-1)
          vt->parser.v.csi.leader[vt->parser.v.csi.leaderlen++] = c;
        break;
      }

      /* else fallthrough */
      vt->parser.v.csi.leader[vt->parser.v.csi.leaderlen] = 0;

      vt->parser.v.csi.argi = 0;
      vt->parser.v.csi.args[0] = CSI_ARG_MISSING;
      vt->parser.state = CSI_ARGS;

      /* fallthrough */
    case CSI_ARGS:
      /* Numerical value of argument */
      if(c >= '0' && c <= '9') {
        if(vt->parser.v.csi.args[vt->parser.v.csi.argi] == CSI_ARG_MISSING)
          vt->parser.v.csi.args[vt->parser.v.csi.argi] = 0;
        vt->parser.v.csi.args[vt->parser.v.csi.argi] *= 10;
        vt->parser.v.csi.args[vt->parser.v.csi.argi] += c - '0';
        break;
      }
      if(c == ':') {
        vt->parser.v.csi.args[vt->parser.v.csi.argi] |= CSI_ARG_FLAG_MORE;
        c = ';';
      }
      if(c == ';') {
        vt->parser.v.csi.argi++;
        vt->parser.v.csi.args[vt->parser.v.csi.argi] = CSI_ARG_MISSING;
        break;
      }

      /* else fallthrough */
      vt->parser.v.csi.argi++;
      vt->parser.intermedlen = 0;
      vt->parser.state = CSI_INTERMED;
    case CSI_INTERMED:
      if(is_intermed(c)) {
        if(vt->parser.intermedlen < INTERMED_MAX-1)
          vt->parser.intermed[vt->parser.intermedlen++] = c;
        break;
      }
      else if(c == 0x1b) {
        /* ESC in CSI cancels */
      }
      else if(c >= 0x40 && c <= 0x7e) {
        vt->parser.intermed[vt->parser.intermedlen] = 0;
        do_csi(vt, c);
      }
      /* else was invalid CSI */

      ENTER_NORMAL_STATE();
      break;

    case OSC_COMMAND:
      /* Numerical value of command */
      if(c >= '0' && c <= '9') {
        if(vt->parser.v.osc.command == -1)
          vt->parser.v.osc.command = 0;
        else
          vt->parser.v.osc.command *= 10;
        vt->parser.v.osc.command += c - '0';
        break;
      }
      if(c == ';') {
        vt->parser.state = OSC;
        string_start = bytes + pos + 1;
        break;
      }

      /* else fallthrough */
      string_start = bytes + pos;
      vt->parser.state = OSC;
      goto string_state;

    case DCS_COMMAND:
      if(vt->parser.v.dcs.commandlen < CSI_LEADER_MAX)
        vt->parser.v.dcs.command[vt->parser.v.dcs.commandlen++] = c;

      if(c >= 0x40 && c<= 0x7e) {
        string_start = bytes + pos + 1;
        vt->parser.state = DCS;
      }
      break;

string_state:
    case OSC:
    case DCS:
      if(c == 0x07 || (c1_allowed && c == 0x9c)) {
        string_fragment(vt, string_start, string_len, true);
        ENTER_NORMAL_STATE();
      }
      break;

    case NORMAL:
      if(vt->parser.in_esc) {
        if(is_intermed(c)) {
          if(vt->parser.intermedlen < INTERMED_MAX-1)
            vt->parser.intermed[vt->parser.intermedlen++] = c;
        }
        else if(c >= 0x30 && c < 0x7f) {
          do_escape(vt, c);
          vt->parser.in_esc = 0;
          ENTER_NORMAL_STATE();
        }
        else {
          DEBUG_LOG("TODO: Unhandled byte %02x in Escape\n", c);
        }
        break;
      }
      if(c1_allowed && c >= 0x80 && c < 0xa0) {
        switch(c) {
        case 0x90: // DCS
          vt->parser.string_initial = true;
          vt->parser.v.dcs.commandlen = 0;
          ENTER_STATE(DCS_COMMAND);
          break;
        case 0x9b: // CSI
          vt->parser.v.csi.leaderlen = 0;
          ENTER_STATE(CSI_LEADER);
          break;
        case 0x9d: // OSC
          vt->parser.v.osc.command = -1;
          vt->parser.string_initial = true;
          ENTER_STATE(OSC_COMMAND);
          break;
        default:
          do_control(vt, c);
          break;
        }
      }
      else {
        size_t eaten = 0;
        if(vt->parser.callbacks && vt->parser.callbacks->text)
          eaten = (*vt->parser.callbacks->text)(bytes + pos, len - pos, vt->parser.cbdata);

        if(!eaten) {
          DEBUG_LOG("libvterm: Text callback did not consume any input\n");
          /* force it to make progress */
          eaten = 1;
        }

        pos += (eaten - 1); // we'll ++ it again in a moment
      }
      break;
    }
  }

  if(string_start)
    string_fragment(vt, string_start, bytes + pos - string_start, false);

  return len;
}

void vterm_parser_set_callbacks(VTerm *vt, const VTermParserCallbacks *callbacks, void *user)
{
  vt->parser.callbacks = callbacks;
  vt->parser.cbdata = user;
}

void *vterm_parser_get_cbdata(VTerm *vt)
{
  return vt->parser.cbdata;
}
