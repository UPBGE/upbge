import bge
import enum

class JoyStickIndex(enum.Enum):
    MANETTE_1 = 0
    MANETTE_2 = 1
    MANETTE_3 = 2
    MANETTE_4 = 3

class JoyStickAxis(enum.Enum):
    GAUCHE_X = 0 # 0 = joysticks gauche x
    GAUCHE_Y = 1 # 1 = joysticks gauche y
    DROITE_X = 2 # 2 = joysticks droit x
    DROITE_Y = 3 # 3 = joysticks droit y
    BOUTON_L2 = 4 # bouton L2 
    BOUTON_R2 = 5 # bouton R2
    # les boutons L2 et R2 disposent d'un niveau de pression
    # c'est pourquoi ils sont considérées comme des axes

class JoyStickButton(enum.Enum):
    BOUTON_A = 0 # 0 = bouton a, crois
    BOUTON_B = 1 # 1 = bouton b, cercle
    BOUTON_X = 2 # 3 = bouton x, carré
    BOUTON_Y = 3 # 2 = bouton y, triangle
    BOUTON_SELECT = 4 # bouton SELECT
    BOUTON_HOME = 5 # bouton HOME
    BOUTON_START = 6 # bouton START
    BOUTON_L3 = 7 # bouton L3
    BOUTON_R3 = 8 # bouton R3
    BOUTON_L1 = 9 # bouton L1
    BOUTON_R1 = 10 # bouton R1
    DPAD_HAUT = 11 # bouton pad haut
    DPAD_BAS = 12 # bouton pad bas
    DPAD_GAUCHE = 13 # bouton pad gauche
    DPAD_DROITE = 14 # bouton pad droit

manette = bge.logic.joysticks[JoyStickIndex.MANETTE_1.value]
print("bouton ", manette.activeButtons)
print("axes ", manette.axisValues)

if manette.axisValues[JoyStickAxis.GAUCHE_X.value] > 0.3: print("lx_droite")

elif manette.axisValues[JoyStickAxis.GAUCHE_X.value] < -0.3: print("lx_gauche")

if manette.axisValues[JoyStickAxis.GAUCHE_Y.value] > 0.3: print("ly_bas")

elif manette.axisValues[JoyStickAxis.GAUCHE_Y.value] < -0.3: print("ly_haut")

if manette.axisValues[JoyStickAxis.DROITE_X.value] > 0.3: print("rx_droite")

elif manette.axisValues[JoyStickAxis.DROITE_X.value] < -0.3: print("rx_gauche")

if manette.axisValues[JoyStickAxis.DROITE_Y.value] > 0.3: print("ry_bas")

elif manette.axisValues[JoyStickAxis.DROITE_Y.value] < -0.3: print("ry_haut")

if JoyStickButton.BOUTON_A.value in manette.activeButtons: print("Bouton A pressé")