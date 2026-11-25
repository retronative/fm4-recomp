#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: migrate-playtest-profile.sh OLD_HOME PRODUCTION_HOME USER_DATA_ROOT
# The launcher supplies resolved storage locations. The migration uses macOS's
# included Perl for flock, symlink-safe checks and same-volume atomic renames.
exec /usr/bin/perl - "$@" <<'PERL'
use strict;
use warnings;
use Fcntl qw(:DEFAULT :flock :mode);
use File::Basename qw(dirname);
use File::Find ();
use File::Temp qw(tempdir tempfile);

sub fail { die "Playtest profile migration: @_\n" }
sub checked_path {
    my ($path) = @_;
    defined($path) && $path =~ m{^/[^/]} && $path !~ m{(?:^|/)(?:\.|\.\.)(?:/|$)|//|[\r\n]}
        or fail("Expected an absolute folder path.");
    $path =~ s{/$}{};
    # System aliases on macOS. Refuse custom symlinks so source and target
    # cannot silently refer back to each other or a disconnected volume.
    $path =~ s{^/(tmp|var)(?=/|$)}{/private/$1};
    my $part = '';
    for (split m{/}, substr($path, 1)) {
        $part .= "/$_";
        -l $part and fail("Symbolic link in $part");
        -e $part && !-d $part and fail("Expected a folder at $part");
    }
    return $path;
}
sub contains_dir {
    my ($ancestor, $p) = @_;
    my @root = stat($ancestor);
    while (1) {
        return 1 if $p eq $ancestor;
        my @s = stat($p);
        return 1 if @root && @s && $s[0] == $root[0] && $s[1] == $root[1];
        return 0 if $p eq '/';
        $p = dirname($p);
    }
}
sub entries {
    my ($dir) = @_;
    opendir(my $d, $dir) or fail("Cannot read $dir: $!");
    my @names = sort grep { $_ ne '.' && $_ ne '..' && $_ ne '.DS_Store' && $_ !~ /^\._/ } readdir($d);
    closedir($d) or fail("Cannot close $dir: $!");
    return @names;
}
sub regular_tree {
    my ($root) = @_;
    File::Find::find({no_chdir => 1, wanted => sub {
        my $p = $File::Find::name;
        my @s = lstat($p);
        @s && (S_ISDIR($s[2]) || S_ISREG($s[2])) or fail("Unsupported file or symbolic link at $p");
        -r $p && (!S_ISDIR($s[2]) || -x $p) or fail("Cannot read $p");
    }}, $root);
}

sub same_contents {
    my ($a, $b) = @_;
    if (-d $a && -d $b) {
        # diff can print every changed file. Its status is sufficient here.
        open(my $reader, '-|', '/usr/bin/diff', '-qr', '-x', '.DS_Store', '-x', '._*', $a, $b)
            or fail("Cannot compare $a and $b");
        1 while <$reader>;
        close($reader);
    } elsif (-f $a && -f $b) { system('/usr/bin/cmp', '-s', $a, $b) }
    else { return 0 }
    my $result = $?;
    return 1 if $result == 0;
    return 0 if $result == 256;
    fail("Cannot verify $a and $b");
}
# Only import a whole entry into an absent destination. A staged account keeps
# each package and its Headers together. Existing accounts never merge by file.
sub copy_if_absent {
    my ($source, $dest) = @_;
    regular_tree($source);
    -l $dest and fail("Symbolic link at $dest");
    if (-e $dest) {
        return 0 unless -d $dest && -d $source;
        opendir(my $d, $dest) or fail("Cannot inspect $dest: $!");
        my @names = grep { $_ ne '.' && $_ ne '..' } readdir($d);
        closedir($d);
        return 0 if @names;
    }
    my $staging = tempdir('.fm4-import-XXXXXXXX', DIR => dirname($dest), CLEANUP => 1);
    my $copy = "$staging/item";
    system('/usr/bin/ditto', '--norsrc', '--noextattr', '--noacl', $source, $copy) == 0
        or fail("Cannot copy $source. The original remains there.");
    regular_tree($copy);
    same_contents($source, $copy) or fail("Could not verify $source. Stop the old game before retrying.");
    rename($copy, $dest) or fail("Cannot install $dest: $!");
    return 1;
}

@ARGV == 3 or fail("Expected old home, new home and configured user-data root.");
umask(0077);
my ($old, $new, $user) = map { checked_path($_) } @ARGV;
if ($old =~ m{^(/Volumes/[^/]+)(?:/|$)}) {
    -d $1 or fail("The old profile volume is unavailable: $1");
}
my $profile = checked_path("$old/shiftglue");
exit 0 unless -d $profile;
-d $new && -d $user or fail("The selected profile or save folder is unavailable. Reconnect its drive.");
my $settings = checked_path("$new/config");
my $source_user = checked_path("$profile/user");
my $source_settings = checked_path("$profile/config");
contains_dir($old, $new) || contains_dir($new, $old) ||
    contains_dir($old, $user) || contains_dir($user, $old) ||
    contains_dir($settings, $user) || contains_dir($user, $settings)
    and fail("Overlapping profile or save locations.");
(-d $source_user || -d $source_settings) or exit 0;

my @locks;
for my $path (sort "$new/.playtest-import.lock", "$user/.fm4-playtest-import.lock") {
    sysopen(my $fh, $path, O_CREAT|O_RDWR|O_NOFOLLOW, 0600) or fail("Cannot open $path: $!");
    -f $fh or fail("Invalid lock at $path");
    flock($fh, LOCK_EX) or fail("Cannot lock $path: $!");
    push @locks, $fh;
}

my $marker = "$user/.fm4-playtest-import-v1";
my $marker_data = "$old\n$new\n$user\n";
if (-e $marker || -l $marker) {
    -f $marker && !-l $marker && -s $marker == length($marker_data)
        or fail("The existing migration record is invalid: $marker");
    open(my $f, '<', $marker) or fail("Cannot read $marker");
    local $/;
    <$f> eq $marker_data or fail("The migration record belongs to different profile locations: $marker");
    exit 0;
}
my @accounts = -d $source_user ? grep { /^[0-9a-f]{16}$/i } entries($source_user) : ();
my @settings = -d $source_settings ? entries($source_settings) : ();
my $awards = "$source_user/achievements";
my $has_awards = -e $awards || -l $awards;
@accounts || @settings || $has_awards or exit 0;

my $conflict = 0;
for my $account (@accounts) {
    my $source = "$source_user/$account";
    -d $source && !-l $source or fail("Invalid old account at $source");
    my $target = "$user/$account";
    next if copy_if_absent($source, $target);
    regular_tree($target);
    next if same_contents($source, $target); # A retry after publishing this account.
    # Setup can create an account for other saved items before the first race.
    # Recover the career if missing, and preserve all its pre-existing packages.
    my ($payload, $header) = ('4D530910/00000001/ForzaProfile', '4D530910/Headers/00000001/ForzaProfile.header');
    if (-f "$source/$payload/ForzaProfile" && -f "$source/$header" &&
            (!-f "$target/$payload/ForzaProfile" || !-f "$target/$header")) {
        for my $part ('4D530910', '4D530910/00000001', '4D530910/Headers', '4D530910/Headers/00000001') {
            my $dir = checked_path("$target/$part");
            mkdir($dir, 0700) || -d $dir or fail("Cannot create $dir: $!");
        }
        # Install data first, then its metadata. A restart only attaches a
        # missing counterpart to a byte-identical first half.
        for my $part ($payload, $header) {
            -e "$target/$part" && !same_contents("$source/$part", "$target/$part")
                and fail("Conflicting incomplete career at $target. Previous save retained at $source.");
        }
        copy_if_absent("$source/$payload", "$target/$payload");
        copy_if_absent("$source/$header", "$target/$header");
        warn "Recovered a missing career in $target. Other old packages remain at $source.\n";
    } else {
        $conflict = 1;
        warn "Kept the existing account at $target. The previous save and its headers remain at $source.\n";
    }
}
if ($has_awards && !$conflict) { copy_if_absent($awards, "$user/achievements") }
if (@settings) {
    mkdir($settings, 0700) || -d $settings or fail("Cannot create $settings: $!");
    for my $name (@settings) {
        copy_if_absent("$source_settings/$name", "$settings/$name");
    }
}
# Publish the marker last; an interrupted import can be retried. Keeping it
# outside the account means a later intentional save/settings deletion sticks.
my ($fh, $tmp_marker) = tempfile('.fm4-marker-XXXXXXXX', DIR => $user, UNLINK => 1);
print {$fh} $marker_data or fail("Cannot write $tmp_marker");
close($fh) or fail("Cannot finish $tmp_marker");
rename($tmp_marker, $marker) or fail("Cannot publish $marker: $!");
PERL
