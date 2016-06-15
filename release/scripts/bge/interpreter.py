import sys
import os
import rlcompleter
import readline
import bge
import code

# Check if a console exits.
if os.isatty(sys.stdin.fileno()):
	# Autocompletion with tab.
	readline.parse_and_bind("tab: complete")

	# BGE defines.
	scene = bge.logic.getCurrentScene()

	# Launch interactive console with current locals.
	code.interact(local=locals())
