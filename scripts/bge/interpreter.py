import sys
import os
import bge
import code

# Check if a console exits.
if os.isatty(sys.stdin.fileno()):
	try:
		import readline
	except ImportError:
		print("Can not enable autocompletion, readline module is missing")
	else:
		import rlcompleter
		# Autocompletion with tab.
		readline.parse_and_bind("tab: complete")

	if sys.platform.startswith("win"):
		print("Python interpreter started. Press Ctrl+C or Ctrl+Z+Enter to quit.")
	else:
		print("Python interpreter started. Press Ctrl+D to quit.")

	# Launch interactive console with current locals.
	code.interact(local=locals())
