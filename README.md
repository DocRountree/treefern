# TreeFern
TreeFern is a lightweight FOSS tool for editing cladograms, written in C++ and based mostly on Qt.

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
 
<img width="1705" height="1414" alt="2026-10-19-135804_1705x1414_scrot" src="https://github.com/user-attachments/assets/41640b73-b5ff-4089-9430-c34cc7d7c54d" /> <img width="1717" height="1420" alt="2026-10-19-135953_1717x1420_scrot" src="https://github.com/user-attachments/assets/bc76fdc9-8de5-4fde-a381-db6876cb9cd9" />

* **Dyamic Labels:** Labels too small to be seen are hidden until zoomed in, and labels dynamically adjust positions to avoid overcrowding. This is the most finicky feature and I will probably be working on it for a while. Label modes can also be toggled between an over-node and beside-node layout, which persists between sessions.

<img width="1705" height="1414" alt="2026-10-19-135804_1705x1414_scrot" src="https://github.com/user-attachments/assets/386afe67-1ea7-4da2-bfbc-ba968de4e8d3" /> <img width="1718" height="1421" alt="2026-10-19-140011_1718x1421_scrot" src="https://github.com/user-attachments/assets/a4ecdf62-4e02-4838-866a-0786b539c4ed" />

* **Dark & Light Modes:** Dark Mode is standard, and light mode can be activated in the view panel - setting persists across sessions.

<img width="1720" height="1421" alt="2026-10-19-135924_1720x1421_scrot" src="https://github.com/user-attachments/assets/18ac35a9-d3bc-4715-8c28-1f896164b4cc" />

* **Search & Navigation:** Navigation can be done entirely by keyboard or entirely by mouse, and clades can be found by a search bar in the side panel.

# Usage

Treefern can be launched from the terminal by running:

>./treefern

Or by clicking in the GUI on the executable file (or a shortcut to it) that gets built during the installation.

In the terminal, a specific file can be opened with TreeFern by running:

>./treefern file.json

The GUI for the program also has a file picker, and can open JSON files that are dragged into the program canvas.

