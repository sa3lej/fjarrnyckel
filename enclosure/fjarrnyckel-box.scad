// Fjärrnyckel enclosure.
//
// Holds a bare ESP32-S3-DevKitC-1 (44 pins) standing on its own pin headers, two MAX98357A amplifier boards
// with headers (front one for the speaker, rear one for the headphones), a 31 x 70 mm boxed speaker and one or two 3.5 mm panel sockets (key, and optionally
// headphones). The board is not screwed down: the black plastic strips of its two pin rows sit on a pair
// of saddles with a channel for the pins, stops at both ends of the strips keep it from sliding when a
// USB plug goes in or out, and a post under the lid keeps it from lifting. The amplifier lies loose in
// its own seat, away from the board's antenna; each amplifier lies loose in its own seat.
//
// Parts: base, lid, and a small lens for the status light (print it in a clear or white filament).
// Print the base as it stands and the lid upside down (flat top on the bed). No supports.
// Four M3 x 8 machine screws go in from below into brass heat-set inserts in the posts under the lid, so
// the top has no screws. (Set inserts = false to drive M3 x 10 self-tapping screws straight into the posts.)
//
// The sizes under "Measure these" are taken from data sheets and typical clone boards.
// Measure your own parts with calipers and correct them before printing.

part = "assembly";   // assembly | open | base | lid | lens | print_lid | cut_post | cut_speaker

$fn = 64;

// ---- Measure these ----------------------------------------------------------
esp_l = 57.0;        // board length, without the USB sockets sticking out
esp_w = 28.0;        // board width (25.4 on the original Espressif board)
esp_t = 1.6;         // board thickness
row_pitch = 25.4;    // distance between the two pin rows: 25.4 on the wide clones (measured on SA3LEJ's "678AB V0.0.1" board), 22.86 on the original
hdr_h = 2.5;         // height of the black plastic strip of the pin headers
hdr_l = 55.9;        // length of that strip (22 pins)
pin_drop = 8.6;      // underside of the board to the pin tips (black strip + pin)
usb_proud = 1.2;     // how far the USB sockets stick out past the board edge
usb_span = 30.0;     // width of the opening over both USB-C sockets together, with room for two plugs' grips
usb_plug_h = 10.0;   // height of that opening
usb_up = 1.7;        // centre of the USB sockets above the top of the board
can_up = 3.3;        // top of the metal can of the radio module above the top of the board
can_from_end = 14;   // centre of that can from the antenna end of the board
led_from_usb = 20;   // status light: distance from the USB edge of the board
led_off = 0;         // status light: sideways offset from the board's centre line
amp_w = 19.5;        // amplifier board, side with the pin row
amp_d = 18.0;        // amplifier board, other side
amp_pins = 7;        // pins in its header
amp_pin_drop = 8.6;  // underside of the amplifier board to its pin tips
spk_w = 31.0;        // speaker box: width
spk_l = 70.0;        //              length
spk_h = 17.0;        //              height
jack_hole = 6.3;     // thread of the 3.5 mm panel sockets
headphones = true;   // second socket hole for headphones

// ---- Box --------------------------------------------------------------------
wall = 2.4;
floor_t = 2.6;
lid_t = 2.6;
radius = 9;          // outer corner radius
edge = 1.8;          // chamfer on the top and bottom edges
fit = 0.3;           // clearance between printed parts
in_w = 116;          // inside width  (x)
in_d = 80;           // inside depth  (y)
in_h = 26;           // inside height (z)
lip = 3.2;           // depth of the lid's locating lip
post_d = 8.4;        // screw posts under the lid
inserts = true;      // brass heat-set inserts in the ends of the posts: ruthex RX-M3x5.7
insert_d = 4.0;      // hole for the insert, as ruthex specifies (try 4.2 if your printer makes holes tight)
insert_l = 6.7;      // depth of that hole: ruthex asks for the insert's 5.7 mm plus 1 mm
screw_pilot = 2.6;   // M3 self-tapping, when inserts = false
screw_clear = 3.4;
head_d = 6.4;        // screw head recess in the floor
head_h = 1.5;
engrave = 0.5;
name = "FJÄRRNYCKEL";
callsign = "SA3LEJ";

// ---- Layout (origin: inside front-left corner of the floor; the rear wall is at y = in_d) ----
pcb_z = pin_drop + 0.9;                         // underside of the board above the floor
esp_cx = 27;                                    // centre line of the board
esp_y1 = in_d - usb_proud;                      // USB edge of the board
esp_y0 = esp_y1 - esp_l;                        // antenna edge
saddle_z = pcb_z - hdr_h;                       // the pin strips rest here
hdr_y0 = esp_y0 + (esp_l - hdr_l) / 2;          // where the pin strips start
usb_z = pcb_z + esp_t + usb_up;
amp_rim = 1.2;
amp_x = 84;                                     // the amplifiers live on the right, clear of the antenna
amp_ys = headphones ? [8, 36] : [8];            // speaker amplifier at the front; headphone amplifier behind it,
                                                // ending 6 mm short of the sockets' bodies
amp_z = amp_pin_drop + 0.9;                     // top of the amplifier's seat
spk_x = 47;
spk_y = (in_d - spk_l) / 2;
spk_lift = in_h - spk_h - 2.2;                  // speaker stands on ribs so its face meets the lid
jack_x = headphones ? [87, 100] : [95];
jack_z = 13;
posts = [[5.2, 5.2], [in_w - 5.2, 5.2], [5.2, in_d - 5.2], [in_w - 5.2, in_d - 5.2]];
led_x = esp_cx + led_off;
led_y = esp_y1 - led_from_usb;
led_slot = [14, 2.6];                           // the light window in the lid
well_z = pcb_z + esp_t + 7;                     // lower edge of the light well
grille_n = 13;
grille_pitch = 4.6;

module rounded(w, d, h, r) {
  hull() for (x = [r, w - r], y = [r, d - r]) translate([x, y, 0]) cylinder(r = r, h = h);
}

// A rounded slab whose lower (bottom = true) or upper edge is chamfered.
module slab(w, d, h, r, c, bottom) {
  hull() {
    translate([0, 0, bottom ? c : 0]) rounded(w, d, h - c, r);
    translate([c, c, bottom ? 0 : h - 0.01]) rounded(w - 2 * c, d - 2 * c, 0.01, r - c);
  }
}

module slot(l, w, h) { hull() for (x = [-(l - w) / 2, (l - w) / 2]) translate([x, 0, 0]) cylinder(d = w, h = h); }

module label(txt, size = 4, spacing = 1.08) {
  linear_extrude(engrave + 0.01) text(txt, size = size, font = "Liberation Sans:style=Bold", halign = "center", valign = "center", spacing = spacing);
}

module base() {
  ow = in_w + 2 * wall; od = in_d + 2 * wall;
  difference() {
    union() {
      difference() {
        translate([-wall, -wall, -floor_t]) slab(ow, od, floor_t + in_h, radius, edge, true);
        rounded(in_w, in_d, in_h + 1, radius - wall);
      }
      // the board's seat: under each pin row a saddle (two rails with a channel for the pins between them)
      // that the black strip rests on, with a stop at each end of the strip
      for (s = [-1, 1]) translate([esp_cx + s * row_pitch / 2, 0, 0]) {
        for (r = [-1, 1]) translate([r * 1.2 - 0.6, hdr_y0, 0]) cube([1.2, hdr_l, saddle_z]);
        for (y = [hdr_y0 - 0.2 - 1.6, hdr_y0 + hdr_l + 0.2]) translate([-1.8, y, 0]) cube([3.6, 1.6, saddle_z + 0.8]);
      }
      // the amplifiers' seats: a block with a channel for the pin row and a rim around the board
      for (amp_y = amp_ys) difference() {
        translate([amp_x, amp_y, 0]) cube([amp_w + 2 * (amp_rim + fit), amp_d + 2 * (amp_rim + fit), amp_z + 1.6]);
        translate([amp_x + amp_rim, amp_y + amp_rim, amp_z]) cube([amp_w + 2 * fit, amp_d + 2 * fit, 2]);
        translate([amp_x + amp_rim + fit + (amp_w - amp_pins * 2.54) / 2 - 0.4, amp_y + amp_rim + fit + 0.2, 0.6]) cube([amp_pins * 2.54 + 0.8, 3.2, amp_z]);
        translate([amp_x - 1, amp_y + amp_rim + fit + 6, amp_z]) cube([amp_rim + 2, amp_d - 9, 2]);   // wires leave towards the speaker and the board
      }
      // the speaker's cradle: two ribs it stands on and four corner guides
      for (y = [spk_y + 10, spk_y + spk_l - 12]) translate([spk_x, y, 0]) cube([spk_w, 2, spk_lift]);
      for (x = [spk_x - fit - 1.6, spk_x + spk_w + fit], y = [spk_y + 4, spk_y + spk_l - 14])
        translate([x, y, 0]) cube([1.6, 10, spk_lift + 6]);
      for (y = [spk_y - fit - 1.6, spk_y + spk_l + fit]) translate([spk_x + spk_w / 2 - 6, y, 0]) cube([12, 1.6, spk_lift + 6]);
    }
    // rear wall: one opening for both USB sockets, big enough for the grips of two plugs side by side
    translate([esp_cx, in_d - 1, usb_z]) rotate([-90, 0, 0]) slot(usb_span, usb_plug_h, wall + 2);
    // socket holes, each with a shallow seat outside so a short thread still takes its nut
    for (x = jack_x) translate([x, in_d - 1, jack_z]) rotate([-90, 0, 0]) { cylinder(d = jack_hole, h = wall + 2); translate([0, 0, 1 + wall - 0.8]) cylinder(d = 12, h = 2); }
    // rear wall lettering
    translate([esp_cx, in_d + wall - engrave, usb_z + 9.2]) rotate([90, 0, 180]) label("USB", 2.6);
    translate([jack_x[0], in_d + wall - engrave, jack_z + 9.5]) rotate([90, 0, 180]) label("KEY", 2.6);
    if (headphones) translate([jack_x[1], in_d + wall - engrave, jack_z + 9.5]) rotate([90, 0, 180]) label("PHONES", 2.6);
    // screws from below, and shallow seats for four stick-on feet
    for (p = posts) translate([p[0], p[1], -floor_t - 1]) {
      cylinder(d = screw_clear, h = floor_t + 2);
      cylinder(d = head_d, h = head_h + 1);
    }
    for (x = [16, in_w - 16], y = [16, in_d - 16]) translate([x, y, -floor_t - 0.01]) cylinder(d = 10.4, h = 0.6);
    translate([in_w / 2, in_d / 2, -floor_t - 0.01]) mirror([1, 0, 0]) label(callsign, 5);
  }
}

module lid() {
  ow = in_w + 2 * wall; od = in_d + 2 * wall;
  can_z = pcb_z + esp_t + can_up;
  difference() {
    union() {
      translate([-wall, -wall, in_h]) slab(ow, od, lid_t, radius, edge, false);
      // locating lip
      difference() {
        translate([fit, fit, in_h - lip]) rounded(in_w - 2 * fit, in_d - 2 * fit, lip + 0.3, radius - wall - fit);
        translate([fit + 1.6, fit + 1.6, in_h - lip - 1]) rounded(in_w - 2 * fit - 3.2, in_d - 2 * fit - 3.2, lip + 1, radius - wall - fit - 1.6);
      }
      // screw posts
      for (p = posts) translate([p[0], p[1], 0.2]) cylinder(d = post_d, h = in_h + 0.1);
      // post that keeps the board down, ending 0.4 mm above the metal can (a 0.5 mm foam pad takes up the gap)
      translate([esp_cx, esp_y0 + can_from_end, can_z + 0.4]) cylinder(d1 = 6, d2 = 9, h = in_h - can_z - 0.1);
      // light well: a funnel from above the status light up to the window. It starts 7 mm above the
      // board so wires can pass under it, and is widest at the bottom to catch the light.
      hull() {
        translate([led_x, led_y, well_z]) slot(led_slot[0] + 3.6, 7.4, 0.01);
        translate([led_x, led_y, in_h]) slot(led_slot[0] + 1.2, led_slot[1] + 1.4, 0.3);
      }
      // frame that meets the speaker's face
      difference() {
        translate([spk_x - 1.6, spk_y - 1.6, in_h - 2.2 + fit]) cube([spk_w + 3.2, spk_l + 3.2, 2.5 - fit]);
        translate([spk_x + 1.5, spk_y + 1.5, in_h - 3]) cube([spk_w - 3, spk_l - 3, 4]);
      }
    }
    for (p = posts) translate([p[0], p[1], -1]) {
      cylinder(d = inserts ? 3.2 : screw_pilot, h = inserts ? 1 + 0.2 + insert_l + 6 : in_h - 1);   // room for the end of the screw
      if (inserts) cylinder(d = insert_d, h = 1 + 0.2 + insert_l);
    }
    // the light window: a narrow well with a ledge near the top that the lens sits on
    hull() {
      translate([led_x, led_y, well_z - 0.01]) slot(led_slot[0] + 1.2, 5.0, 0.01);
      translate([led_x, led_y, in_h - 0.01]) slot(led_slot[0] - 1.2, led_slot[1] - 1.0, 0.02);
    }
    translate([led_x, led_y, in_h - 0.02]) slot(led_slot[0] - 1.2, led_slot[1] - 1.0, lid_t);
    translate([led_x, led_y, in_h + lid_t - 1.6]) slot(led_slot[0], led_slot[1], 2);
    // speaker grille
    for (i = [0 : grille_n - 1])
      translate([spk_x + spk_w / 2, spk_y + spk_l / 2 + (i - (grille_n - 1) / 2) * grille_pitch, in_h - 1]) slot(spk_w - 7, 2.4, lid_t + 2);
    // lettering
    translate([esp_cx, 11, in_h + lid_t - engrave]) label(name, 3.2, 1.08);
    translate([in_w - 19, 11, in_h + lid_t - engrave]) label(callsign, 3.4, 1.1);
  }
}

// The lens drops into the light window from above. Print it flat in clear or white filament.
module lens() { slot(led_slot[0] - 2 * 0.2, led_slot[1] - 2 * 0.2, 1.6); }

// ---- The parts that go inside, for the pictures only -----------------------------------------
module parts() {
  // ESP32 board on its headers
  translate([esp_cx, 0, pcb_z]) {
    color("#16233a") translate([-esp_w / 2, esp_y0, 0]) cube([esp_w, esp_l, esp_t]);
    color("#c9c9c9") translate([-9, esp_y0 + 5, esp_t]) cube([18, 20, can_up]);
    color("#c9c9c9") for (s = [-1, 1]) translate([s * 6.6 - 4.5, esp_y1 - 7, esp_t]) cube([9, 7 + usb_proud, 3.2]);
    color("#222") for (s = [-1, 1]) translate([s * row_pitch / 2 - 1.27, hdr_y0, -hdr_h]) cube([2.54, hdr_l, hdr_h]);
    color("#d4af37") for (s = [-1, 1], i = [0 : 21]) translate([s * row_pitch / 2 - 0.32, hdr_y0 + 0.95 + i * 2.54, -pin_drop]) cube([0.64, 0.64, pin_drop]);
    color("#f6f6f6") translate([led_off - 1.7, esp_y1 - led_from_usb - 1.7, esp_t]) cube([3.4, 3.4, 1]);
  }
  // amplifiers
  for (amp_y = amp_ys) {
  color("#0b4f9e") translate([amp_x + amp_rim + fit, amp_y + amp_rim + fit, amp_z]) cube([amp_w, amp_d, 1.6]);
  color("#2f8f3a") translate([amp_x + amp_rim + 5, amp_y + amp_rim + 9, amp_z + 1.6]) cube([8, 7, 8]);
  }
  // speaker
  color("#1b1b1b") translate([spk_x, spk_y, spk_lift]) cube([spk_w, spk_l, spk_h]);
  // inserts and screws
  if (inserts) for (p = posts) translate([p[0], p[1], 0]) {
    color("#c8a24a") translate([0, 0, 0.25]) difference() { cylinder(d = 4.6, h = 5.5); translate([0, 0, -1]) cylinder(d = 2.5, h = 8); }
    color("#9aa0a6") { translate([0, 0, -floor_t + head_h - 8 + 8]) translate([0, 0, 0]) cylinder(d = 2.9, h = 0.01); translate([0, 0, -floor_t + head_h]) cylinder(d = 2.9, h = 8); translate([0, 0, -floor_t - 0.3]) cylinder(d = 5.6, h = head_h + 0.3); }
  }
  // wires from the board to the amplifier and the sockets, through the tunnel under the speaker
  color("#d9822b") for (i = [0 : 4]) translate([esp_cx + 14, in_d / 2 - 8 + i * 2.2, 2.2]) rotate([0, 90, 0]) cylinder(d = 1.4, h = amp_x - esp_cx - 12, $fn = 12);
  color("#3aa655") for (i = [0 : 2]) translate([esp_cx + 14, in_d / 2 + 8 + i * 2.2, 2.2]) rotate([0, 90, 0]) cylinder(d = 1.4, h = jack_x[0] - esp_cx - 14, $fn = 12);
  // sockets
  for (x = jack_x) translate([x, in_d + wall + 2, jack_z]) rotate([90, 0, 0]) {
    color("#b9b9b9") cylinder(d = 6, h = wall + 4);
    color("#b9b9b9") translate([0, 0, 0]) cylinder(d = 9.5, h = 2, $fn = 6);
    color("#222") translate([0, 0, wall + 2]) cylinder(d = 8, h = 14);
  }
}

if (part == "base") base();
else if (part == "lid") lid();
else if (part == "print_lid") translate([0, in_d, in_h + lid_t]) rotate([180, 0, 0]) lid();
else if (part == "lens") lens();
else if (part == "open") { color("#22344f") base(); parts(); }
else if (part == "cut_post") difference() {      // the box cut through the two left-hand screws
  union() { color("#22344f") base(); color("#31486b") lid(); parts(); }
  translate([posts[0][0], -50, -50]) cube([300, 300, 300]);
}
else if (part == "cut_speaker") difference() {   // the box cut across the middle, seen from the front
  union() { color("#22344f") base(); color("#31486b") lid(); parts(); }
  translate([-50, -300 + in_d / 2 - 4, -50]) cube([300, 300, 300]);
}
else { color("#22344f") base(); color("#22344f") lid(); color("#f4ecd8") translate([led_x, led_y, in_h + lid_t - 1.6]) lens(); parts(); }
