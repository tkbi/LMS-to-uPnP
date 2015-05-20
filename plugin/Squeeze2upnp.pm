package Plugins::UPnPBridge::Squeeze2upnp;

use strict;

use Proc::Background;
use File::ReadBackwards;
use File::Spec::Functions;
use Data::Dumper;

use Slim::Utils::Log;
use Slim::Utils::Prefs;
use XML::Simple;

my $prefs = preferences('plugin.upnpbridge');
my $log   = logger('plugin.upnpbridge');

my $squeeze2upnp;
my $binary;

sub binaries {
	my $os = Slim::Utils::OSDetect::details();
	
	if ($os->{'os'} eq 'Linux') {

		if ($os->{'osArch'} =~ /x86_64/) {
			return qw(squeeze2upnp-x86-64);
		}
		if ($os->{'binArch'} =~ /i386/) {
			return qw(squeeze2upnp-x86 squeeze2upnp-x86-static);
		}
		if ($os->{'binArch'} =~ /arm/) {
			return qw(squeeze2upnp-armv6hf squeeze2upnp-armv6hf-static);
		}

		# fallback to offering all linux options for case when architecture detection does not work
		return qw(squeeze2upnp-x86-64 squeeze2upnp-x86 squeeze2upnp-x86-static squeeze2upnp-armv6hf squeeze2upnp-armv6hf-static);
	}
	
	if ($os->{'os'} eq 'Darwin') {
		return qw(squeeze2upnp-osx-multi);
	}
	
	if ($os->{'os'} eq 'Windows') {
		return qw(squeeze2upnp-win.exe);
	}	
	
=comment	
		if ($os->{'isWin6+'} ne '') {
			return qw(squeeze2upnp-win.exe);
		} else {
			return qw(squeeze2upnp-winxp.exe);
		}	
	}
=cut	
	
}

sub bin {
	my $class = shift;

	my @binaries = $class->binaries;

	if (scalar @binaries == 1) {
		return $binaries[0];
	}

	if (my $b = $prefs->get("bin")) {
		for my $bin (@binaries) {
			if ($bin eq $b) {
				return $b;
			}
		}
	}

	return $binaries[0] =~ /squeeze2upnp-osx/ ? $binaries[0] : undef;
}

sub start {
	my $class = shift;

	my $bin = $class->bin || do {
		$log->warn("no binary set");
		return;
	};

	my @params;
	my $logging;

	push @params, ("-Z", "-k");
	
	if ($prefs->get('debugs') ne '') {
		push @params, ("-d", $prefs->get('debugs') . "=debug");
	}
		
	if ($prefs->get('autosave')) {
		push @params, ("-I");
	}
	
	if ($prefs->get('eraselog')) {
		unlink $class->logFile;
	}

	if ($prefs->get('logging') || $prefs->get('debugs') ne '') {
		push @params, ("-f", $class->logFile);
		$logging = 1;
	}
	
	if (-e $class->configFile || $prefs->get('autosave')) {
		push @params, ("-x", $class->configFile);
	}
	
	if ($prefs->get('opts') ne '') {
		push @params, split(/\s+/, $prefs->get('opts'));
	}
	
	if (Slim::Utils::OSDetect::details()->{'os'} ne 'Windows') {
		my $exec = catdir(Slim::Utils::PluginManager->allPlugins->{'UPnPBridge'}->{'basedir'}, 'Bin', $bin);
		$exec = Slim::Utils::OSDetect::getOS->decodeExternalHelperPath($exec);
			
		if (!((stat($exec))[2] & 0100)) {
			$log->warn('executable not having \'x\' permission, correcting');
			chmod (0555, $exec);
		}	
	}	
	
	my $path = Slim::Utils::Misc::findbin($bin) || do {
		$log->warn("$bin not found");
		return;
	};

	my $path = Slim::Utils::OSDetect::getOS->decodeExternalHelperPath($path);
			
	if (!-e $path) {
		$log->warn("$bin not executable");
		return;
	}
	
	push @params, @_;

	if ($logging) {
		open(my $fh, ">>", $class->logFile);
		print $fh "\nStarting Squeeze2upnp: $path @params\n";
		close $fh;
	}
	
	eval { $squeeze2upnp = Proc::Background->new({ 'die_upon_destroy' => 1 }, $path, @params); };

	if ($@) {

		$log->warn($@);

	} else {
		Slim::Utils::Timers::setTimer($class, Time::HiRes::time() + 1, sub {
			if ($squeeze2upnp && $squeeze2upnp->alive) {
				$log->debug("$bin running");
				$binary = $path;
			}
			else {
				$log->debug("$bin NOT running");
			}
		});
	}
}

sub stop {
	my $class = shift;

	if ($squeeze2upnp && $squeeze2upnp->alive) {
		$log->info("killing squeeze2upnp");
		$squeeze2upnp->die;
		# not sure it is needed, but squeeze2upnp takes some time to stop when killed properly
		# it might as be a problem to have that in the main loop
		$squeeze2upnp->wait;
	}
}

sub alive {
	return ($squeeze2upnp && $squeeze2upnp->alive) ? 1 : 0;
}

sub wait {
	$log->info("waiting for squeeze2upnp to end");
	$squeeze2upnp->wait
}

sub restart {
	my $class = shift;

	$class->stop;
	$class->start;
}

sub logFile {
	return catdir(Slim::Utils::OSDetect::dirsFor('log'), "upnpbridge.log");
}

sub configFile {
	return catdir(Slim::Utils::OSDetect::dirsFor('prefs'), $prefs->get('configfile'));
}

sub logHandler {
	my ($client, $params, undef, undef, $response) = @_;

	$response->header("Refresh" => "10; url=" . $params->{path} . ($params->{lines} ? '?lines=' . $params->{lines} : ''));
	$response->header("Content-Type" => "text/plain; charset=utf-8");

	my $body = '';
	my $file = File::ReadBackwards->new(logFile());
	
	if ($file){

		my @lines;
		my $count = $params->{lines} || 100;

		while ( --$count && (my $line = $file->readline()) ) {
			unshift (@lines, $line);
		}

		$body .= join('', @lines);

		$file->close();			
	};

	return \$body;
}

sub configHandler {
	my ($client, $params, undef, undef, $response) = @_;

	$response->header("Content-Type" => "text/xml; charset=utf-8");

	my $body = '';
	
	if (-e configFile) {
		open my $fh, '<', configFile;
		
		read $fh, $body, -s $fh;
		close $fh;
	}	

	return \$body;
}

1;
