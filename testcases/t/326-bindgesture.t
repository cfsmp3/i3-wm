#!perl
# vim:ts=4:sw=4:expandtab
#
# Please read the following documents before working on tests:
# • https://build.i3wm.org/docs/testsuite.html
#   (or docs/testsuite)
#
# • https://build.i3wm.org/docs/lib-i3test.html
#   (alternatively: perldoc ./testcases/lib/i3test.pm)
#
# • https://build.i3wm.org/docs/ipc.html
#   (or docs/ipc)
#
# • https://i3wm.org/downloads/modern_perl_a4.pdf
#   (unless you are already familiar with Perl)
#
# Verifies that gesture bindings (bindgesture) and their removals
# (unbindgesture) are validated like other configuration directives and that i3 keeps working with gesture bindings,
# even when the X server provides no gesture devices.
#
use i3test i3_autostart => 0;
use File::Temp qw(tempfile);

sub check_config {
    my ($config) = @_;
    my ($fh, $tmpfile) = tempfile(UNLINK => 1);
    print $fh "font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1\n";
    print $fh $config;
    $fh->flush;
    my $output = qx(DISPLAY= i3 -C -c $tmpfile 2>&1);
    my $retval = $?;
    close($fh);
    return ($retval >> 8, $output);
}

################################################################################
# 1: valid gesture bindings
################################################################################

my ($ret, $out) = check_config(<<'EOT');
set $pad Second Pad
bindgesture swipe:3:left workspace prev
bindgesture swipe:4 nop four fingers
bindgesture swipe:right nop any number of fingers
bindgesture swipe:3:inward nop never triggers
bindgesture --exact pinch:2:inward+up nop exact
bindgesture --input-device="$pad" --exact --no-warn pinch:outward nop device
bindgesture --exact --exact --no-warn --no-warn swipe:2:up nop repeated options
bindgesture --input-device=Touchpad --input-device=* swipe:2:down nop last device wins
bindgesture swipe:2:right --exact nop options after the gesture belong to the command
bindgesture --input-device=* pinch:clockwise+inward nop all devices
bindgesture pinch:any nop any pinch
bindgesture hold nop hold
mode "gestures" {
    bindgesture swipe:3:left workspace prev
}
EOT
is($ret, 0, 'valid gesture bindings: exit code == 0');
is($out, '', 'valid gesture bindings: no output');

################################################################################
# 2: invalid gesture bindings
################################################################################

for my $line (
    'bindgesture swipex:3 nop',
    'bindgesture swipe:0:left nop',
    'bindgesture swipe:10:left nop',
    'bindgesture swipe::left nop',
    'bindgesture swipe:3:sideways nop',
    'bindgesture swipe:3:left+sideways nop',
    'bindgesture swipe:3x nop',
    'bindgesture swipe:3:left:extra nop',
    'bindgesture --frobnicate swipe:3 nop',
    'bindgesture --input-device swipe:3 nop',
    'bindgesture --input-device= swipe:3 nop',
    'bindgesture swipe:3',
    'bindgesture') {
    ($ret, $out) = check_config("$line\n");
    is($ret, 1, "$line: exit code == 1");
    like($out, qr/ERROR/, "$line: error reported");
}

################################################################################
# 3: duplicate gesture bindings
################################################################################

($ret, $out) = check_config(<<'EOT');
bindgesture pinch:2:inward+up nop first
bindgesture pinch:2:up+inward+up nop second
EOT
is($ret, 1, 'duplicate gesture bindings: exit code == 1');
like($out, qr/ERROR: *Duplicate gesture binding/, 'duplicate gesture bindings reported');

($ret, $out) = check_config(<<'EOT');
bindgesture --input-device=* swipe:left nop first
bindgesture --no-warn swipe:left nop second
EOT
is($ret, 0, 'duplicate gesture binding with --no-warn: exit code == 0');
is($out, '', 'duplicate gesture binding with --no-warn: no output');

($ret, $out) = check_config(<<'EOT');
bindgesture swipe:left nop first
bindgesture --exact swipe:left nop exact
bindgesture swipe:3:left nop three fingers
bindgesture --input-device="Touchpad" swipe:left nop device
mode "other" {
    bindgesture swipe:left nop other mode
}
EOT
is($ret, 0, 'no duplicate gesture bindings: exit code == 0');
is($out, '', 'no duplicate gesture bindings: no output');

################################################################################
# 4: removing gesture bindings
################################################################################

($ret, $out) = check_config(<<'EOT');
set $pad Second Pad
bindgesture swipe:3:left nop removed
unbindgesture swipe:3:left+any+left
bindgesture --exact --input-device="$pad" pinch:up+inward nop removed
unbindgesture --input-device=Touchpad --input-device="$pad" --exact pinch:inward+up
bindgesture --input-device=* swipe:right nop removed
unbindgesture swipe:any+right
bindgesture swipe:3:left nop added again
mode "other" {
    bindgesture swipe:left nop other mode
    unbindgesture swipe:left
}
EOT
is($ret, 0, 'gesture binding removals: exit code == 0');
is($out, '', 'gesture binding removals: no output');

for my $config (
    "unbindgesture swipe:left\nbindgesture swipe:left nop later\n",
    "bindgesture swipe:left nop\nunbindgesture swipe:3:left\n",
    "bindgesture swipe:left nop\nunbindgesture --exact swipe:left\n",
    "bindgesture swipe:left nop\nunbindgesture --continuous swipe:left\n",
    "bindgesture swipe:left nop\nunbindgesture --input-device=Touchpad swipe:left\n",
    "bindgesture --input-device=Touchpad swipe:left nop\nunbindgesture --input-device=touchpad swipe:left\n",
    "bindgesture swipe:left nop\nunbindgesture pinch:left\n",
    "bindgesture swipe:left nop\nmode \"other\" {\n    unbindgesture swipe:left\n}\n",
    "bindgesture swipe:left nop\nunbindgesture swipe:left\nunbindgesture swipe:left\n",
    "bindgesture swipe:left nop\nunbindgesture --no-warn swipe:left\n",
    "bindgesture swipe:left nop\nunbindgesture swipe:left nop\n",
    "bindgesture swipe:left nop\nunbindgesture swipe:left --exact\n",
    "unbindgesture swipe:3:sideways\n",
    "unbindgesture\n") {
    ($ret, $out) = check_config($config);
    is($ret, 1, "removal: exit code == 1 for: $config");
    like($out, qr/ERROR/, "removal: error reported for: $config");
}

################################################################################
# 5: i3 keeps running with gesture bindings, also when switching modes and
# reloading.
################################################################################

my $config = <<'EOT';
# i3 config file (v4)
font -misc-fixed-medium-r-normal--13-120-75-75-C-70-iso10646-1

bindgesture swipe:3:left nop swipe
bindgesture pinch:inward nop pinch
mode "gestures" {
    bindgesture swipe:right nop swipe in mode
}
EOT

my $pid = launch_with_config($config);

my $i3 = i3(get_socket_path());
$i3->connect->recv;
is($i3->message(12, "")->recv->{name}, 'default', 'binding mode is default');

cmd 'mode gestures';
is($i3->message(12, "")->recv->{name}, 'gestures', 'binding mode is gestures');

cmd 'reload';
is($i3->message(12, "")->recv->{name}, 'default', 'binding mode is default after reload');

does_i3_live;

exit_gracefully($pid);

done_testing;
