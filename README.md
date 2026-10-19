# TreeFern
TreeFern is a lightweight FOSS tool for editing cladograms, written in C++.

It stores cladograms as JSON files, with live file synchronization and autosave. 

TreeFern supports adding daughter/sister clades, adding metadata information to clades, and cutting/pasting clades onto new mother clades.



# Installation

To install TreeFern, use the following steps:

1. Save the [main.cpp](main.cpp) file and [CMakeLists.txt](CMakeLists.txt) file in a directory of your choice. Make sure you have CMake installed and that your machine can compile C++ code.
2. In that directory, to build the program, run the command:

   >cmake -B build
   
3. To compile the program, run the command:

   >cmake --build build -j

4. TreeFern can now be started with the command:

   >./build/treefern

# Features

* **Two View Layouts**
  * **Standard** Tapered branching tree structure in which heavier child branches curve away from their parent branches, inspired by OneZoom.org's viewer.
  * **Disks** Nested circular packing where child clades are inscribed discs inside their parent clade discs.
* **Dyamic Labels:** Labels too small to be seen are hidden until zoomed in, and labels dynamically adjust positions to avoid overcrowding. This is the most finicky feature and I will probably be working on it for a while. Label modes can also be toggled between an over-node and beside-node layout, which persists between sessions.
* **Search & Navigation:** Navigation can be done entirely by keyboard, entirely by mouse, and clades can be found by a search bar in the side panel.
* **Dark & Light Modes:** Dark Mode is standard, and light mode can be activated in the view panel - setting persists across sessions.


# Usage

Treefern can be launched from the terminal by running:

>./treefern

Or by clicking in the GUI on the executable file (or a shortcut to it) that gets built during the installation.

In the terminal, a specific file can be opened with TreeFern by running:

>./treefern file.json

The GUI for the program also has a file picker, and can open JSON files that are dragged into the program canvas.


# Example


