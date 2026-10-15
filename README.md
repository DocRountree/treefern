# TreeFern
TreeFern is a lightweight FOSS tool for editing cladograms, written in C++.

It stores cladograms as JSON files, and both tracks changes made in the file and commits changes made in the GUI in real-time, and saves them automatically. 

TreeFern supports adding daughter/sister clades, editing clade names and information, and cutting/pasting clades onto new mother clades.




# Installation

Installing TreeFern is quite straightforward.

1. Save the [main.cpp](main.cpp) file and [CMakeLists.txt](CMakeLists.txt) file in a directory of your choice. Make sure you have CMake installed and that your machine can compile C++ code.
2. In that directory, to build the program, run the command:

   >cmake -B build
   
3. To compile the program, run the command:

   >cmake --build build -j

4. TreeFern can now be started with the command:

   >./build/treefern


# Usage

TreeFern reads .JSON files to build a recursive cladogram, and stores written information about clades according to user preference. (See the [JSON Template](template.json) for more information.)

<img width="1627" height="1322" alt="1" src="https://github.com/user-attachments/assets/9b12d441-bd94-4f71-a649-5ff3ca46df6b" />


Functionality can be accessed via keyboard shortcuts, or via GUI buttons in the side panel which appears when a clade is selected by a single left-click.

<img width="2015" height="1346" alt="2" src="https://github.com/user-attachments/assets/165afa40-7f22-4cdc-9a0b-ea1464f8dfb9" />


Double-clicking a clade opens the Edit menu (also accessible by clicking "Edit" in the side-panel, or by the keyboard shortcut 'e').

<img width="2188" height="1349" alt="3" src="https://github.com/user-attachments/assets/e6998241-9ee5-4964-9054-876f835c587f" />
