# Claditor
Claditor is a GUI cladogram editor written in C++.

It stores cladograms as JSON files, and both tracks and commits changes to the file in real-time.

Claditor supports adding daughter/sister clades, editing clade names and information, and cutting/pasting clades onto new mother clades.




# Installation

Installing Claditor is quite straightforward.

1. Save the main.cpp file and CMakeLists.txt file in a directory of your choice.
2. Run the command:

   >cmake --build build -j

3. The program can now be started with the command:

   >./build/claditor


# Usage

Claditor reads .JSON files to build a recursive cladogram, and stores written information about clades according to user instructions. (See JSON Template for more information.)

<img width="1453" height="1375" alt="2026-10-14-205954_1453x1375_scrot" src="https://github.com/user-attachments/assets/590a33b6-8b49-407d-9ca7-cc2f493b65b4" />

Functionality can be accessed via keyboard shortcuts, or via GUI buttons in the side panel, which appear when a clade of selected by a single left-click.

<img width="1764" height="1381" alt="2026-10-14-210040_1764x1381_scrot" src="https://github.com/user-attachments/assets/92611c9f-fcf2-480d-a1cd-563dbabc47a7" />

Double-clicking a clade opens the Edit menu (also accessible by clicking "Edit" in the side-panel, or by the keyboard shortcut 'e').

<img width="1998" height="1382" alt="2026-10-14-210108_1998x1382_scrot" src="https://github.com/user-attachments/assets/7dad3803-a886-424e-bd32-fa2521433b85" />
