// Remote CW keyer box for an ESP32-S3-DevKitC-1 on a screw-terminal expansion
// board, a MAX98357A amplifier, a 31 x 70 mm boxed speaker, two PJ-392 3.5 mm
// sockets and one speed potentiometer.
//
// Parts: base, lid and a speaker strap. Print the base as it stands, the lid
// upside down (flat top on the bed) and the strap flat. No supports needed.
//
// The sizes under "Measured parts" are estimates from product photos. Measure
// the real parts with calipers and correct them before printing.

part = "assembly";   // assembly | open | base | lid | strap | print_lid

$fn = 56;

// ---- Measured parts (CHECK THESE) -----------------------------------------
board_w = 60;        // expansion board, side that carries the USB end
board_d = 70;        // expansion board, other side
board_t = 1.6;       // expansion board thickness
board_stack = 26;    // underside of expansion board to top of the ESP32
usb_w = 30;          // width of the slot for both USB-C ports
usb_h = 9;           // height of that slot
usb_z = 15;          // centre of the USB ports above the board underside
usb_cx = 0;          // sideways offset of the USB ports from board centre
spk_w = 31;          // speaker width
spk_l = 70;          // speaker length
spk_h = 17;          // speaker depth
amp_w = 19;          // amplifier board
amp_d = 18;
jack_hole = 6.3;     // PJ-392 thread
pot_hole = 7.3;      // potentiometer bushing
pot_tab = 2.8;       // anti-rotation tab hole (0 to leave it out)
pot_tab_r = 7.8;     // tab distance from shaft centre

// ---- Box -------------------------------------------------------------------
wall = 2.4;
floor_t = 2.0;
lid_t = 2.4;
radius = 7;          // outer corner radius
fit = 0.3;           // clearance between printed parts
in_w = 120;          // inside width  (x)
in_d = 80;           // inside depth  (y)
in_h = 38;           // inside height (z)
stand = 4;           // board standoff above the floor
lip = 4;             // lid lip depth
post_d = 8;          // lid screw posts
screw_pilot = 2.6;   // M3 self-tapping; use 4.0 for heat-set inserts
engrave = 0.6;
callsign = "SA3LEJ";

// ---- Layout (origin: inside front-left corner of the floor) ---------------
board_x = 10;
board_y = in_d - board_d - 0.5;
zone_cx = 96;                       // centre line of the right-hand zone
jack_x = [zone_cx - 10, zone_cx + 10];
jack_z = 13;
pot_x = zone_cx;
pot_z = 13;
spk_cy = in_d / 2;
strap_span = 38;                    // distance between the two strap posts
posts = [[4.5, 4.5], [in_w - 4.5, 4.5], [4.5, in_d - 4.5], [in_w - 4.5, in_d - 4.5]];

module rounded(w, d, h, r) {
  hull() for (x = [r, w - r], y = [r, d - r]) translate([x, y, 0]) cylinder(r = r, h = h);
}

module label(txt, size = 4.5) {
  linear_extrude(engrave + 0.01) text(txt, size = size, font = "Liberation Sans:style=Bold", halign = "center", valign = "center");
}

module base() {
  difference() {
    translate([-wall, -wall, -floor_t]) rounded(in_w + 2 * wall, in_d + 2 * wall, floor_t + in_h, radius);
    rounded(in_w, in_d, in_h + 1, radius - wall);
    // rear wall: USB slot and the two key sockets
    translate([board_x + board_w / 2 + usb_cx - usb_w / 2, in_d - 1, stand + usb_z - usb_h / 2])
      hull() for (x = [usb_h / 2, usb_w - usb_h / 2]) translate([x, 0, usb_h / 2]) rotate([-90, 0, 0]) cylinder(d = usb_h, h = wall + 2);
    for (x = jack_x) translate([x, in_d - 1, jack_z]) rotate([-90, 0, 0]) cylinder(d = jack_hole, h = wall + 2);
    // front wall: speed potentiometer
    translate([pot_x, -wall - 1, pot_z]) rotate([-90, 0, 0]) cylinder(d = pot_hole, h = wall + 2);
    if (pot_tab > 0) translate([pot_x - pot_tab_r, -1.4, pot_z]) rotate([-90, 0, 0]) cylinder(d = pot_tab, h = 2.4);
    // labels
    translate([jack_x[0], in_d + wall - engrave, jack_z + 9]) rotate([90, 0, 180]) label("KEY", 4);
    translate([jack_x[1], in_d + wall - engrave, jack_z + 9]) rotate([90, 0, 180]) label("PADDLE", 4);
    translate([board_x + board_w / 2 + usb_cx, in_d + wall - engrave, stand + usb_z + usb_h / 2 + 5]) rotate([90, 0, 180]) label("USB", 4);
    translate([pot_x, -wall + engrave, pot_z + 14]) rotate([90, 0, 0]) label("SPEED", 4.5);
    // recesses for stick-on feet
    for (x = [10, in_w - 10], y = [10, in_d - 10]) translate([x, y, -floor_t - 0.01]) cylinder(d = 11, h = 0.8);
  }
  // lid screw posts
  for (p = posts) translate([p[0], p[1], 0]) difference() {
    cylinder(d = post_d, h = in_h);
    translate([0, 0, in_h - 14]) cylinder(d = screw_pilot, h = 15);
  }
  // board rests and corner guides
  for (cx = [0, 1], cy = [0, 1]) {
    px = board_x + cx * board_w;
    py = board_y + cy * board_d;
    sx = cx ? -1 : 1;
    sy = cy ? -1 : 1;
    translate([px, py, 0]) {
      translate([sx > 0 ? 0 : -7, sy > 0 ? 0 : -7, 0]) cube([7, 7, stand]);
      translate([sx > 0 ? -2.2 : 0.2, sy > 0 ? -2.2 : -8, 0]) cube([2, 10.2, stand + board_t + 3]);
      if (cy == 0) translate([sx > 0 ? -2.2 : -8, -2.2, 0]) cube([10.2, 2, stand + board_t + 3]);
    }
  }
  // cradle for the amplifier board
  translate([zone_cx - amp_w / 2 - 1.6, 30, 0]) difference() {
    cube([amp_w + 3.2, amp_d + 3.2, 4]);
    translate([1.4, 1.4, 1.2]) cube([amp_w + 0.4, amp_d + 0.4, 4]);
    translate([4, -1, 1.2]) cube([amp_w - 4.8, amp_d + 6, 4]);
  }
}

module grille() {
  step = 4.6;
  for (ix = [-3:3], iy = [-6:6]) {
    x = ix * step + (iy % 2 == 0 ? 0 : step / 2);
    y = iy * step * 0.866;
    if (pow(x / 12.5, 2) + pow(y / 22, 2) <= 1) translate([x, y, -1]) cylinder(d = 3, h = lid_t + 2, $fn = 6);
  }
}

module lid() {
  difference() {
    union() {
      translate([-wall, -wall, in_h]) rounded(in_w + 2 * wall, in_d + 2 * wall, lid_t, radius);
      // lip
      translate([0, 0, in_h - lip]) difference() {
        translate([fit, fit, 0]) rounded(in_w - 2 * fit, in_d - 2 * fit, lip, radius - wall - fit);
        translate([fit + 1.6, fit + 1.6, -1]) rounded(in_w - 2 * fit - 3.2, in_d - 2 * fit - 3.2, lip + 2, max(1, radius - wall - fit - 1.6));
        for (p = posts) translate([p[0], p[1], -1]) cylinder(d = post_d + 2, h = lip + 2);
      }
      // rim that locates the speaker
      translate([zone_cx, spk_cy, in_h - 2]) difference() {
        translate([-spk_w / 2 - 1.8, -spk_l / 2 - 1.8, 0]) cube([spk_w + 3.6, spk_l + 3.6, 2]);
        translate([-spk_w / 2 - 0.3, -spk_l / 2 - 0.3, -1]) cube([spk_w + 0.6, spk_l + 0.6, 4]);
        translate([-spk_w / 2 - 3, -8, -1]) cube([spk_w + 6, 16, 4]);
      }
      // posts for the speaker strap
      for (s = [-1, 1]) translate([zone_cx + s * strap_span / 2, spk_cy, in_h - spk_h]) difference() {
        cylinder(d = 6.5, h = spk_h);
        translate([0, 0, -1]) cylinder(d = screw_pilot, h = 11);
      }
    }
    // lid screws, countersunk
    for (p = posts) translate([p[0], p[1], in_h - 1]) {
      cylinder(d = 3.4, h = lid_t + 2);
      translate([0, 0, lid_t + 1 - 1.7]) cylinder(d1 = 3.4, d2 = 6.6, h = 1.71);
    }
    translate([zone_cx, spk_cy, in_h]) grille();
    translate([board_x + board_w / 2, in_d / 2 + 6, in_h + lid_t - engrave]) label(callsign, 11);
    translate([board_x + board_w / 2, in_d / 2 - 10, in_h + lid_t - engrave]) label("REMOTE CW", 5);
  }
}

module strap() {
  difference() {
    hull() for (s = [-1, 1]) translate([s * strap_span / 2, 0, 0]) cylinder(d = 8, h = 2.4);
    for (s = [-1, 1]) translate([s * strap_span / 2, 0, -1]) cylinder(d = 3.4, h = 5);
  }
}

// ---- Mock-ups of the bought parts, for the pictures only -------------------
module parts() {
  translate([board_x, board_y, stand]) {
    color("#151515") cube([board_w, board_d, board_t]);
    color("#2f9e44") for (x = [2, board_w - 9.5]) translate([x, 6, board_t]) cube([7.5, board_d - 16, 10]);
    color("#222") for (x = [14, board_w - 16.5]) translate([x, 8, board_t]) cube([2.5, board_d - 14, 8.5]);
    color("#0b0b0b") translate([board_w / 2 - 14, board_d - 57, board_t + 8.5]) cube([28, 57, 1.6]);
    color("#c9c9c9") translate([board_w / 2 - 9, board_d - 57 - 6, board_t + 10.1]) cube([18, 25.5, 3.2]);
    color("#dddddd") for (x = [-9.5, 9.5]) translate([board_w / 2 + x - 4.5, board_d - 7.3, board_t + 10.1]) cube([9, 7.5, 3.2]);
  }
  color("#7b3fc4") translate([zone_cx - amp_w / 2, 31.6, 1.2]) cube([amp_w, amp_d, 1.6]);
  for (x = jack_x) translate([x, in_d + wall + 4.5, jack_z]) rotate([90, 0, 0]) {
    color("#d4a017") cylinder(d = 6, h = 6);
    color("#d4a017") translate([0, 0, 4.5 - 2.2]) cylinder(d = 9, h = 2.2, $fn = 6);
    color("#111") translate([0, 0, 4.5 + wall]) cylinder(d = 8.5, h = 13);
  }
  translate([pot_x, -wall, pot_z]) rotate([90, 0, 0]) {
    color("#b8b8b8") cylinder(d = 7, h = 7);
    color("#1a1a1a") translate([0, 0, 4]) cylinder(d1 = 21, d2 = 18, h = 15);
    color("#e8e8e8") translate([-0.8, 3, 19]) cube([1.6, 6.5, 0.4]);
    color("#b8b8b8") translate([0, 0, -wall - 10]) cylinder(d = 17, h = 10);
  }
  color("#101010") translate([zone_cx - spk_w / 2, spk_cy - spk_l / 2, in_h - spk_h]) cube([spk_w, spk_l, spk_h]);
}

shell = "#3d4a5c";
if (part == "base") base();
if (part == "lid") lid();
if (part == "print_lid") translate([0, in_d, in_h + lid_t]) rotate([180, 0, 0]) lid();
if (part == "strap") strap();
if (part == "assembly") { color(shell) base(); color(shell) lid(); parts(); }
if (part == "open") {
  color(shell) base();
  parts();
  translate([0, 0, 55]) { color(shell) lid(); color("#3d4a5c") translate([zone_cx, spk_cy, in_h - spk_h - 2.4]) strap(); }
}
