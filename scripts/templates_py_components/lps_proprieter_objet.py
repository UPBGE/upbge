import bge

# Récupération de la scène active
scene_actif = bge.logic.getCurrentScene() 
# Liste de tous les objets de la scène
obj = scene_actif.objects 

def lister_obj():
    """Cette fonction liste tous les objets présents dans la scène active."""
    for objet in obj:
        # Affiche le nom de chaque objet
        print(objet.name)

def proprieter_obj():
    """Cette fonction affiche le nom d'un objet spécifique et ses propriétés."""
    # Récupération de l'objet nommé "Cam" dans la scène
    OBJ = obj["Cam"] 
    # Récupération des noms des propriétés de l'objet
    prop_obj = OBJ.getPropertyNames()
    # Affiche le nom de l'objet
    print(OBJ.name)
    # Parcours de chaque propriété de l'objet
    for prop_nom in prop_obj:
        # Récupération de la valeur de la propriété
        prop_valeur = OBJ[prop_nom]
        # Détermination du type de la valeur de la propriété
        prop_type = type(prop_valeur)
        # Affichage du type, du nom et de la valeur de la propriété
        print(f"{prop_type}, {prop_nom} = {prop_valeur}")

# Appel de la fonction pour lister les objets
lister_obj()
# Appel de la fonction pour afficher les propriétés de l'objet "Cam"
proprieter_obj()

# Propriété de "PythonComponent"

composant_obj = obj["mon_objet"].components['Nom_component']
for i in composant_obj.args:
    print(i) # affiche les valeurs des arguments de la fonction

composant_obj.args['PV'] = 0
print(composant_obj)