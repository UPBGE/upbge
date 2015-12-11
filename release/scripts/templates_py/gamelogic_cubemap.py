# code from https://en.wikibooks.org/wiki/GLSL_Programming/Blender/Reflecting_Surfaces
# and https://en.wikibooks.org/wiki/GLSL_Programming/Blender/Diffuse_Reflection
# This shader works only with 1 Light Source. You can use a for loop to take into
# account multiple light sources (gl_LightSource[i]). The code works only for 1
# cubemap texture. You have to adapt it to take into account mutliple textures
# (shader.setSampler(textureNameInTheShader, texture_index))
# At the end of the code you can set diffuse to 1 to have diffuse lighting.
# To have ambient lighting, you can add vec4(0.2, 0.2, 0.2, 1.0) to color
# in the Vertex Shader (gl_LightSource[i].ambient seems not working)
# For more details, see https://en.wikibooks.org/wiki/GLSL_Programming/Blender

import bge

cont = bge.logic.getCurrentController()

VertexShader = """
         varying vec4 color;
         uniform int diffuse;
         uniform mat4 viewMatrix; // world to view transformation
         uniform mat4 viewMatrixInverse; 
            // view to world transformation

         varying vec3 viewDirection; // direction in world space 
            // in which the viewer is looking
         varying vec3 normalDirection; // normal vector in world space 
        
         void main()
         {
            vec4 positionInViewSpace = gl_ModelViewMatrix * gl_Vertex;
               // transformation of gl_Vertex from object coordinates 
               // to view coordinates

            vec4 viewDirectionInViewSpace = positionInViewSpace 
               - vec4(0.0, 0.0, 0.0, 1.0);
               // camera is always at (0,0,0,1) in view coordinates;
               // this is the direction in which the viewer is looking 
               // (not the direction to the viewer)
            
            viewDirection = 
               vec3(viewMatrixInverse * viewDirectionInViewSpace);
               // transformation from view coordinates 
               // to world coordinates
               
            vec3 normalDirectionInViewSpace = 
               gl_NormalMatrix * gl_Normal;
               // transformation of gl_Normal from object coordinates 
               // to view coordinates

            normalDirection = normalize(vec3(
               vec4(normalDirectionInViewSpace, 0.0) * viewMatrix));
               // transformation of normal vector from view coordinates 
               // to world coordinates with the transposed 
               // (multiplication of the vector from the left) of 
               // the inverse of viewMatrixInverse, which is viewMatrix
            if (bool(diffuse)) {
                vec3 lightDirection;
                float attenuation;
                if (0.0 == gl_LightSource[0].position.w) 
                   // directional light?
                {
                   attenuation = 1.0; // no attenuation
                   lightDirection = 
                      normalize(vec3(gl_LightSource[0].position));
                } 
                else // point light or spotlight (or other kind of light) 
                {
                   vec3 vertexToLightSource = 
                      vec3(gl_LightSource[0].position 
                      - gl_ModelViewMatrix * gl_Vertex);
                   float distance = length(vertexToLightSource);
                   attenuation = 1.0 / distance; // linear attenuation 
                   lightDirection = normalize(vertexToLightSource);
     
                   if (gl_LightSource[0].spotCutoff <= 90.0) // spotlight?
                   {
                      float clampedCosine = max(0.0, dot(-lightDirection, 
                         gl_LightSource[0].spotDirection));
                      if (clampedCosine < gl_LightSource[0].spotCosCutoff) 
                         // outside of spotlight cone?
                      {
                         attenuation = 0.0;
                      }
                      else
                      {
                         attenuation = attenuation * pow(clampedCosine, 
                            gl_LightSource[0].spotExponent);
                      }
                   }
                }
                vec3 diffuseReflection = attenuation 
                   * vec3(gl_LightSource[0].diffuse) 
                   * max(0.0, dot(normalDirection, lightDirection));
     
                color = vec4(diffuseReflection, 1.0);
            }
            else {
                color = vec4(0.0);
            }
            gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
         }
"""

FragmentShader = """
         varying vec3 viewDirection;
         varying vec3 normalDirection;
         varying vec4 color;
         uniform samplerCube cubeMap;
         uniform int diffuse;

         void main()
         {
            vec3 reflectedDirection = reflect(viewDirection, 
               normalize(normalDirection));
            gl_FragColor = bool(diffuse)? textureCube(cubeMap, vec3(reflectedDirection.x, -reflectedDirection.z, reflectedDirection.y)) * color :
            textureCube(cubeMap, vec3(reflectedDirection.x, -reflectedDirection.z, reflectedDirection.y));
               // usually this would be: gl_FragColor = 
               // textureCube(cubeMap, reflectedDirection);
               // however, Blender appears to reshuffle the faces a bit
         }
"""

mesh = cont.owner.meshes[0]
for mat in mesh.materials:
    shader = mat.getShader()
    if shader != None:
        if not shader.isValid():
            shader.setSource(VertexShader, FragmentShader, 1)
            shader.setSampler('cubeMap', 0)
        viewMatrix = bge.logic.getCurrentScene().active_camera.world_to_camera
        shader.setUniformMatrix4('viewMatrix', viewMatrix)
        viewMatrixInverse = bge.logic.getCurrentScene().active_camera.camera_to_world
        shader.setUniformMatrix4('viewMatrixInverse', viewMatrixInverse)
        shader.setUniform1i('diffuse', 0) # to have diffuse component in the shader, set to 1
