cf-profile
==========
This simple perl script is aimed at giving more information about cf-agent runs. It draws out a execution tree
together with timings and classes augmented so that you can see clearly the order of execution together with what
part of your policy cf-agent is spending most time on.

Example:

# cf-agent -v | ./cf-profile.pl

Requires Perl module Time::HiRes.
