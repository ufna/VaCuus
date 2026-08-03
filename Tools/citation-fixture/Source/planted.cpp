// Tools/citation-fixture/Source/planted.cpp -- the citation gate's planted failures.
//
// NOT COMPILED, and not reachable from any .Build.cs: this tree exists only so that
// Tools/citation_check.py can be SEEN to fail before it is allowed to pass. One
// planted citation per violation class, plus a control that must be left alone.
//
// The EXPECT list in Tools/citation_check.py names the line numbers below. Editing
// this file without editing EXPECT is the failure the self-test exists to make loud.

// PLANT 1 -- STALE_RANGE. The cited file resolves (it is right next to this one) but
// has nowhere near that many lines. This is what a citation looks like once its
// target shrank, or its content moved out to another file (fixture_target.h:400).
void PlantedStaleRange();

// PLANT 2 -- UNRESOLVED. The cited file has never existed under any configured root.
// This is the shape a rename leaves behind: the comment goes on naming the old file
// forever, and nothing but this check ever notices
// (ThisFileHasNeverExisted.cpp:7).
void PlantedUnresolved();

// CONTROL -- must NOT be reported. fixture_target.h really does have a line 3, so a
// gate that flags this one has stopped telling stale from sound
// (fixture_target.h:3).
void ControlCleanCitation();
