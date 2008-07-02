Patchutils news
---------------

0.2.31 (stable)

	Several small bugs were fixed.

0.2.30 (stable)

	Some minor parsing bugs were fixed, the documentation was
	clarified, and a new option was added to lsdiff/filterdiff for
	selecting patches based on the order in which they appear.

0.2.29 (stable)

	Several small bugs were fixed.

0.2.28 (stable)

	Some format conversion bugs were fixed.

0.2.27 (stable)

	An option was added to lsdiff to treating empty files as absent.
	The filterdiff and interdiff utilities now handle patches containing
	embedded null characters.  The dehtmldiff utility was improved
	slightly.

0.2.26 (stable)

	Some build fixes were made.

0.2.25 (stable)

	A new filterdiff option for displaying patch filenames.  A new
	splitdiff option to make it behave more like diffsplit.  Other
	minor bugfixes.

0.2.24 (stable)

	A new filterdiff option for removing timestamps.  New grepdiff options
	-E and -f.  Minor bugfixes.

0.2.23 (stable)

	Minor bugfixes.

0.2.22 (stable)

	Several small bugs were fixed.  A new option was added to splitdiff
	to split out every file-level patch, and a new option was added to
	grepdiff to display matching hunks.

0.2.21 (stable)

	Several bugs were fixed in flipdiff and dehtmldiff.

0.2.20 (stable)

	A new tool was added for exchanging the order of two patches.

0.2.19 (stable)

	Several bug fixes were made, and a new program was added for
	extracting a diff from an HTML page.

0.2.18 (stable)

	Support for ISO 8601 timezones.

0.2.17 (stable)

	Support for conversion between unified and context formats was
	added.

0.2.16 (stable)

	A new option (--annotate) was added to filterdiff, and interdiff
	can now strip context lines from patches.

0.2.15 (stable)

	The recountdiff and unwrapdiff tools were added for fixing
	broken diffs.

0.2.14 (stable)

	Hunk-level filtering was added to filterdiff.

0.2.13 (stable)

	In this release, some minor bugs were fixed, a new option
	(--no-revert-omitted) was added to interdiff, and two new
	options (--strip and --addprefix) were added to filterdiff,
	lsdiff, and grepdiff.

0.2.12 (stable)

	Several small issues were addressed.

0.2.11 (stable)

	In this release, filterdiff, lsdiff and grepdiff now understand
	context diff format, and several rediff bugs were fixed.

0.2.10 (stable)

	The last of the known interdiff bugs are now fixed.

0.2.9 (stable)

	A small bug concerning new files in 'cvs diff' output was fixed.

0.2.8 (stable)

	Interdiff and filterdiff were fixed to handle filenames with
	spaces in them.  A new tool, grepdiff, was added.

0.2.7 (stable)

	A new option for showing added and removed files was added to
	lsdiff, and some portability fixes were made.

0.2.6 (stable)

	A bug concerning zero-context patches was fixed, and the man
	pages are now automatically generated from DocBook XML.

0.2.5 (stable)

	Some silly bugs in lsdiff and combinediff were fixed.

0.2.4 (stable)

	Support for patches created using interdiff and combinediff.

0.2.3 (stable)

	Small enhancements for rediff and two new tools (lsdiff and
	splitdiff) were added.

0.2.2 (stable)

	A new tool called combinediff has been added, and the test
	suite has been enhanced.

0.2.1 (stable)

	Substantial speed improvements for interdiff and a new tool
	(rediff) for correcting hand-edited patches were added.

0.2.0 (stable)

	There is now an option for ignoring whitespace in interdiff.

0.1.5 (devel)

	A tiny perl script for fixing cvs diff results and minor
	clean-ups added.

0.1.4 (devel)

	Some code cleanups.

0.1.3 (devel)

	Support for decompressing patches (by popular demand).

0.1.2 (devel)

	Minor build fixes.

0.1.1 (devel)

	Interdiff and extractdiff have been merged into patchutils,
	which now uses autoconf.

0.1.0 (devel)

	There are new commandline options for controlling the amount
	of context displayed.

Interdiff news
--------------

0.0.10 (stable)

	A corner case and a silly bug have been fixed.

0.0.9 (stable)

	Handling of non-seekable streams.

0.0.8 (stable)

	Some parameter parsing bugs have been fixed.  Leading './'
	pathname components are ignored for filename comparison.

0.0.7 (stable)

	A new -p option, similar to patch(1), and numerous bugfixes.

0.0.5 (stable)

	Support for patches created using gendiff.

0.0.4 (stable)

	Another corner case has been fixed, and some test cases added.
