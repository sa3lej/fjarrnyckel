// Remote VFO knob in the spirit of the Icom RC-28: a sloped desk unit with a
// large weighted tuning knob, two function keys and a wide PTT key, all on
// Cherry MX switches, driven by an ESP32-S3 that wfview sees as an RC-28.
//
// Parts: body (print it standing on its back face, or upside down with
// supports for the slope), bottom plate (flat), knob (upside down, pause at
// the washer layer), keycaps (flat side down).
//
// Sizes marked CHECK are estimates; measure the real encoder, washer and
// switches before printing.

part = "assembly";   // assembly | exploded | body | bottom | knob | keycaps

$fn = 72;

// ---- Measured parts (CHECK) --------------------------------------------------
enc_body_d = 39;      // optical encoder body diameter
enc_shaft_d = 6;      // encoder shaft
enc_flange_holes = 30;// encoder mounting hole circle (M3)
washer_od = 46;       // steel washer cast into the knob
washer_id = 24;
washer_t = 3;
mx_cut = 14;          // Cherry MX plate cut-out
stab_offset = 11.9;   // 2u stabiliser wire housings from centre
usb_w = 12;           // one USB-C port
usb_h = 7;

// ---- Body --------------------------------------------------------------------
W = 112;              // width  (x)
D = 150;              // depth  (y, front = 0)
H0 = 30;              // height at the front
slope = 11;           // top slope in degrees
wall = 2.6;
top_t = 3.0;
plate_t = 3.0;
radius = 10;
callsign = "SA3LEJ";

function top_z(y) = H0 + y * tan(slope);

knob_at = [W / 2, 100];
knob_d = 58;
knob_h = 24;
f_keys = [[W / 2 - 19, 50, "F1"], [W / 2 + 19, 50, "F2"]];
ptt_at = [W / 2, 22];
leds = [[W / 2 - 36, 50], [W / 2 + 36, 50], [W / 2 + 38, 100]];
post_xy = [[9, 9], [W - 9, 9], [9, D - 9], [W - 9, D - 9]];

module footprint(inset = 0, h = 1) {
  hull() for (x = [radius, W - radius], y = [radius, D - radius])
    translate([x, y, 0]) cylinder(r = radius - inset, h = h);
}

module under_slope(dz = 0) {
  // everything below the sloped top plane, lowered by dz
  translate([-50, 0, 0]) rotate([90, 0, 90]) linear_extrude(W + 100)
    polygon([[-50, -100], [D + 50, -100], [D + 50, top_z(D + 50) - dz], [-50, top_z(-50) - dz]]);
}

module on_top(x, y, lift = 0) {
  translate([x, y, top_z(y) + lift]) rotate([slope, 0, 0]) children();
}

module body_solid() {
  intersection() { footprint(0, 200); under_slope(0); }
}

module body() {
  difference() {
    body_solid();
    translate([0, 0, -1]) intersection() { footprint(wall, 200); under_slope(top_t - 1); }
    // knob shaft and encoder mounting holes
    on_top(knob_at[0], knob_at[1], -5) {
      cylinder(d = 10, h = 20, center = true);                 // bushing for the shaft
      for (a = [0:120:240]) rotate(a) translate([enc_flange_holes / 2, 0, 0]) cylinder(d = 3.3, h = 20, center = true);
    }
    // key cut-outs
    for (k = f_keys) on_top(k[0], k[1], -5) cube([mx_cut, mx_cut, 20], center = true);
    on_top(ptt_at[0], ptt_at[1], -5) {
      cube([mx_cut, mx_cut, 20], center = true);
      for (s = [-1, 1]) translate([s * stab_offset, 0, 0]) cube([7, 12.3, 20], center = true);
    }
    // LED windows
    for (l = leds) on_top(l[0], l[1], -5) cylinder(d = 3.2, h = 20, center = true);
    // USB-C on the back
    translate([W / 2 - usb_w / 2, D - wall - 1, plate_t + 8]) cube([usb_w, wall + 2, usb_h]);
    // engraving
    on_top(W / 2, 140, -0.6) linear_extrude(1) text(callsign, size = 6, font = "Liberation Sans:style=Bold", halign = "center", valign = "center");
  }
  // screw posts for the bottom plate
  for (p = post_xy) translate([p[0], p[1], 0]) difference() {
    intersection() { cylinder(d = 8, h = top_z(p[1])); under_slope(0.1); }
    translate([0, 0, -1]) cylinder(d = 2.6, h = 14);
  }
}

module bottom() {
  difference() {
    footprint(wall + 0.3, plate_t);
    for (p = post_xy) translate([p[0], p[1], -1]) {
      cylinder(d = 3.4, h = plate_t + 2);
      cylinder(d1 = 6.6, d2 = 3.4, h = 1.7);
    }
    for (x = [16, W - 16], y = [16, D - 16]) translate([x, y, -0.01]) cylinder(d = 11, h = 0.8);
  }
}

module knob() {
  difference() {
    union() {
      cylinder(d = knob_d, h = knob_h - 4);
      translate([0, 0, knob_h - 4]) cylinder(d1 = knob_d, d2 = knob_d - 3, h = 4);
    }
    // knurled skirt
    for (a = [0:6:359]) rotate(a) translate([knob_d / 2 - 0.4, 0, 2]) cylinder(d = 1.4, h = knob_h - 7, $fn = 8);
    // finger dimple
    translate([knob_d * 0.24, 0, knob_h + 15]) sphere(d = 36);
    // shaft bore with flat
    translate([0, 0, -1]) difference() { cylinder(d = enc_shaft_d + 0.2, h = 16); translate([1.9, -5, 0]) cube([10, 10, 17]); }
    // hidden steel washer for flywheel weight (pause the print here)
    translate([0, 0, 5]) difference() { cylinder(d = washer_od + 0.6, h = washer_t + 0.4); cylinder(d = washer_id - 0.6, h = washer_t + 1); }
  }
}

module keycap(label, w = 18) {
  difference() {
    hull() {
      translate([-w / 2, -9, 0]) cube([w, 18, 1]);
      translate([-w / 2 + 1.5, -7.5, 7]) cube([w - 3, 15, 1]);
    }
    translate([0, 0, 7.4]) linear_extrude(1) text(label, size = w > 20 ? 6 : 4.5, font = "Liberation Sans:style=Bold", halign = "center", valign = "center");
  }
}

module keycaps() {
  translate([0, 0, 0]) keycap("F1");
  translate([25, 0, 0]) keycap("F2");
  translate([60, 0, 0]) keycap("PTT", 37);
}

// ---- Pictures -------------------------------------------------------------------
shell = "#3d4a5c";
module placed(explode = 0) {
  color(shell) body();
  color(shell) translate([0, 0, -explode]) bottom();
  on_top(knob_at[0], knob_at[1], 2 + explode) color("#161616") knob();
  for (k = concat(f_keys, [[ptt_at[0], ptt_at[1]]])) on_top(k[0], k[1], explode * 0.3) color("#5a5f66") translate([-7, -7, 0]) cube([14, 14, 5.5]);
  for (k = f_keys) on_top(k[0], k[1], 5 + explode * 0.6) color("#2a2a2a") keycap(k[2]);
  on_top(ptt_at[0], ptt_at[1], 5 + explode * 0.6) color("#b3261e") keycap("PTT", 37);
  for (l = leds) on_top(l[0], l[1], 0.2) color("#ff9a2e") cylinder(d = 3, h = 0.6);
  if (explode > 0) {
    color("#0b0b0b") translate([W / 2 - 17, D - 75, plate_t + 3]) cube([34, 70, 1.6]);
    on_top(knob_at[0], knob_at[1], -32) color("#9aa0a6") cylinder(d = enc_body_d, h = 26);
  }
}

if (part == "assembly") placed(0);
if (part == "exploded") placed(45);
if (part == "body") body();
if (part == "bottom") bottom();
if (part == "knob") knob();
if (part == "keycaps") keycaps();
