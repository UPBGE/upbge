import bge

touche = bge.events
clavier = bge.logic.keyboard.inputs

# Utilisation de "inputs"
def Cls_inputs():
    if loc_clavier[touche.AKEY].active:
        print("gg")
    if loc_clavier[touche.MIDDLEMOUSE].released:
        print("mouse")

Cls_inputs()