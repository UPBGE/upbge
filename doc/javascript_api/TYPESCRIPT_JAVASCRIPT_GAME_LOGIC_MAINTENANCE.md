# Manutenção do TypeScript/JavaScript como Game Logic

Este documento explica como dar manutenção no suporte a TypeScript e JavaScript como lógica de jogo no UPBGE.

## Visão Geral

O UPBGE possui um motor V8 JavaScript embutido que executa scripts JavaScript/TypeScript em controllers, similar aos controllers Python. O sistema suporta:

- **JavaScript puro**: Executado diretamente pelo V8
- **TypeScript**: Compilado automaticamente para JavaScript antes da execução
- **APIs BGE**: Acesso completo às funcionalidades do game engine via objeto global `bge`

## Arquitetura

### Fluxo de Execução

```
Blender Logic Editor
    ↓
SCA_JavaScriptController (cria controller)
    ↓
[Se TypeScript] KX_TypeScriptCompiler::Compile()
    ↓
V8 Engine (compila e executa JavaScript)
    ↓
KX_V8Bindings (expõe APIs BGE)
    ↓
Game Logic executada
```

### Componentes Principais

1. **SCA_JavaScriptController** - Controller que executa scripts JS/TS
2. **KX_V8Engine** - Wrapper do motor V8 JavaScript
3. **KX_V8Bindings** - Bindings C++ → JavaScript (APIs BGE)
4. **KX_TypeScriptCompiler** - Compilador TypeScript → JavaScript

## Estrutura de Arquivos

```
source/gameengine/
├── GameLogic/
│   └── SCA_JavaScriptController.cpp/h    # Controller JavaScript
├── Ketsji/
│   ├── KX_V8Engine.cpp/h                 # Wrapper do motor V8
│   ├── KX_V8Bindings.cpp/h               # Bindings C++ → JS
│   ├── KX_TypeScriptCompiler.cpp/h      # Compilador TypeScript
│   └── KX_V8Init.cpp                     # Inicialização do V8
```

## Componentes Detalhados

### 1. SCA_JavaScriptController

**Localização:** `source/gameengine/GameLogic/SCA_JavaScriptController.cpp`

#### Responsabilidades

- Gerencia o ciclo de vida do script (compilação, execução)
- Integra com o sistema de sensors/actuators do BGE
- Suporta modo Script e modo Module (futuro)

#### Fluxo de Compilação

1. **Verifica se é TypeScript** (`m_use_typescript`)
2. **Se TypeScript**: Chama `KX_TypeScriptCompiler::Compile()`
3. **Cria contexto V8** para o controller
4. **Inicializa bindings** via `KX_V8Bindings::InitializeBindings()`
5. **Compila script** JavaScript com V8
6. **Armazena script compilado** para execução

#### Fluxo de Execução

1. **Trigger** é chamado quando sensor é ativado
2. **Verifica se precisa recompilar** (`m_bModified`)
3. **Executa script compilado** no contexto V8
4. **Limpa triggered sensors**

### 2. KX_TypeScriptCompiler

**Localização:** `source/gameengine/Ketsji/KX_TypeScriptCompiler.cpp`

#### Responsabilidades

- Compila código TypeScript para JavaScript
- Gera arquivo `.d.ts` com definições de tipos BGE
- Usa `tsc` (TypeScript Compiler) externo

#### Processo de Compilação

1. **Verifica disponibilidade** do `tsc` via `IsAvailable()`
2. **Cria arquivo temporário** `.ts` com o código TypeScript
3. **Gera `bge_upbge.d.ts`** no mesmo diretório com definições de tipos
4. **Adiciona referência** `/// <reference path="bge_upbge.d.ts" />` no início
5. **Executa `tsc`** com flags: `--target ES2020 --module none`
6. **Lê arquivo `.js`** gerado
7. **Remove arquivos temporários**

#### BGE_DTS_CONTENT

Definições TypeScript dos tipos BGE (linhas 82-164):

- `BGEGameObject` - Interface para objetos de jogo
- `BGEScene` - Interface para cenas
- `BGEController` - Interface para controllers
- `BGESensor` - Interface para sensors
- `BGEActuator` - Interface para actuators
- `BGEVehicle` - Interface para veículos físicos
- `BGECharacter` - Interface para personagens físicos
- `bge` - Objeto global com namespaces `logic`, `events`, `constraints`

**IMPORTANTE**: Este conteúdo é **duplicado** em `text_lsp_ts.cc` para o editor. Mantenha ambos sincronizados!

### 3. KX_V8Engine

**Localização:** `source/gameengine/Ketsji/KX_V8Engine.cpp/h`

#### Responsabilidades

- Gerencia o motor V8 (singleton)
- Cria e gerencia contextos V8
- Executa código JavaScript
- Reporta exceções

#### Inicialização

- `KX_V8Engine::Initialize()` - Inicializa V8 (chamado uma vez)
- `KX_V8Engine::Shutdown()` - Encerra V8 (chamado no final)
- `CreateDefaultContext()` - Cria contexto padrão
- `CreateContext()` - Cria novo contexto para cada controller

### 4. KX_V8Bindings

**Localização:** `source/gameengine/Ketsji/KX_V8Bindings.cpp/h`

#### Responsabilidades

- Expõe objetos C++ do BGE para JavaScript
- Cria wrappers JavaScript para objetos BGE
- Implementa funções e propriedades acessíveis via JS

#### Estrutura de Bindings

##### Namespace `bge`

```javascript
bge = {
  logic: { ... },
  events: { ... },
  constraints: { ... }
}
```

##### `bge.logic`

- `getCurrentController()` → `BGEController | null`
- `getCurrentScene()` → `BGEScene | null`
- `getCurrentControllerObject()` → `BGEGameObject | null`

##### `bge.events`

Constantes para input:
- `WKEY`, `SKEY`, `AKEY`, `DKEY` (números)
- `ACTIVE`, `JUSTACTIVATED`, `JUSTRELEASED` (números)

##### `bge.constraints`

- `setGravity(x, y, z)` - Define gravidade da cena
- `getVehicleConstraint(id)` - Obtém veículo por ID
- `createVehicle(chassis)` - Cria novo veículo
- `getCharacter(obj)` - Obtém personagem de um objeto

##### Wrappers de Objetos

Cada tipo de objeto BGE tem um wrapper JavaScript:

- **GameObject**: Propriedades (`name`, `position`, `rotation`, `scale`, `has_physics`) e métodos (`setPosition`, `setRotation`, `setScale`, `applyForce`, `getVelocity`, `rayCast`, etc.)
- **Scene**: Propriedades (`objects`, `activeCamera`, `gravity`) e métodos (`get(name)`)
- **Controller**: Propriedades (`owner`, `sensors`, `actuators`) e métodos (`activate`, `deactivate`)
- **Sensor**: Propriedades (`positive`, `events`)
- **Actuator**: Propriedades (`name`)
- **Vehicle**: Métodos para controle de veículos físicos
- **Character**: Métodos para controle de personagens físicos

## Guias de Manutenção

### Adicionar Nova Função ao `bge.logic`

1. **Edite `KX_V8Bindings.h`**:
   - Adicione declaração da função callback:
     ```cpp
     static void GetNovaFuncao(const v8::FunctionCallbackInfo<v8::Value> &args);
     ```

2. **Edite `KX_V8Bindings.cpp`**:
   - Implemente a função:
     ```cpp
     void KX_V8Bindings::GetNovaFuncao(const FunctionCallbackInfo<Value> &args)
     {
       Isolate *isolate = args.GetIsolate();
       // Obter controller atual
       extern SCA_JavaScriptController *g_currentJavaScriptController;
       if (!g_currentJavaScriptController) {
         args.GetReturnValue().SetNull();
         return;
       }
       
       // Sua lógica aqui
       // ...
       
       // Retornar valor
       args.GetReturnValue().Set(/* valor */);
     }
     ```
   - Registre a função em `SetupLogicObject()`:
     ```cpp
     void KX_V8Bindings::SetupLogicObject(Local<Context> context)
     {
       // ...
       logic_template->Set(String::NewFromUtf8Literal(isolate, "novaFuncao"),
                           FunctionTemplate::New(isolate, GetNovaFuncao));
     }
     ```

3. **Atualize `BGE_DTS_CONTENT`** em `KX_TypeScriptCompiler.cpp`:
   ```cpp
   "declare const bge: {\n"
   "  logic: {\n"
   "    getCurrentController(): BGEController | null;\n"
   "    novaFuncao(): TipoRetorno; // Adicione aqui\n"
   "  };\n"
   ```

4. **Atualize `BGE_DTS_CONTENT`** em `text_lsp_ts.cc` (sincronize!)

5. **Recompile e teste**.

### Adicionar Nova Propriedade a GameObject

1. **Edite `KX_V8Bindings.h`**:
   - Adicione getter (e setter se necessário):
     ```cpp
     static void GameObjectGetNovaPropriedade(v8::Local<v8::Name> property,
                                              const v8::PropertyCallbackInfo<v8::Value> &info);
     static void GameObjectSetNovaPropriedade(v8::Local<v8::Name> property,
                                              v8::Local<v8::Value> value,
                                              const v8::PropertyCallbackInfo<void> &info);
     ```

2. **Edite `KX_V8Bindings.cpp`**:
   - Implemente getter:
     ```cpp
     void KX_V8Bindings::GameObjectGetNovaPropriedade(Local<Name> property,
                                                      const PropertyCallbackInfo<Value> &info)
     {
       Isolate *isolate = info.GetIsolate();
       Local<Object> self = info.This();
       KX_GameObject *obj = GetGameObjectFromWrapper(self);
       if (!obj) {
         return;
       }
       
       // Obter valor da propriedade
       // ...
       
       info.GetReturnValue().Set(/* valor */);
     }
     ```
   - Registre em `CreateGameObjectWrapper()`:
     ```cpp
     template->SetAccessor(String::NewFromUtf8Literal(isolate, "novaPropriedade"),
                          GameObjectGetNovaPropriedade,
                          GameObjectSetNovaPropriedade);
     ```

3. **Atualize `BGE_DTS_CONTENT`**:
   ```cpp
   "interface BGEGameObject {\n"
   "  // ... propriedades existentes\n"
   "  novaPropriedade: tipo; // Adicione aqui\n"
   "}\n"
   ```

4. **Sincronize com `text_lsp_ts.cc`**

5. **Recompile e teste**.

### Adicionar Novo Método a GameObject

1. **Edite `KX_V8Bindings.h`**:
   ```cpp
   static void GameObjectNovoMetodo(const v8::FunctionCallbackInfo<v8::Value> &args);
   ```

2. **Edite `KX_V8Bindings.cpp`**:
   - Implemente o método:
     ```cpp
     void KX_V8Bindings::GameObjectNovoMetodo(const FunctionCallbackInfo<Value> &args)
     {
       Isolate *isolate = args.GetIsolate();
       Local<Object> self = args.This();
       KX_GameObject *obj = GetGameObjectFromWrapper(self);
       if (!obj) {
         return;
       }
       
       // Validar argumentos
       if (args.Length() < 1) {
         isolate->ThrowException(String::NewFromUtf8Literal(isolate, "Argumentos insuficientes"));
         return;
       }
       
       // Sua lógica aqui
       // ...
       
       // Retornar valor (se necessário)
       args.GetReturnValue().Set(/* valor */);
     }
     ```
   - Registre em `CreateGameObjectWrapper()`:
     ```cpp
     template->Set(String::NewFromUtf8Literal(isolate, "novoMetodo"),
                  FunctionTemplate::New(isolate, GameObjectNovoMetodo));
     ```

3. **Atualize `BGE_DTS_CONTENT`**:
   ```cpp
   "interface BGEGameObject {\n"
   "  // ... métodos existentes\n"
   "  novoMetodo(arg1: tipo1, arg2?: tipo2): TipoRetorno;\n"
   "}\n"
   ```

4. **Sincronize com `text_lsp_ts.cc`**

5. **Recompile e teste**.

### Adicionar Novo Tipo de Objeto (ex: BGENewType)

1. **Crie wrapper em `KX_V8Bindings.h`**:
   ```cpp
   static v8::Local<v8::Object> CreateNewTypeWrapper(v8::Isolate *isolate, CppNewType *obj);
   static CppNewType *GetNewTypeFromWrapper(v8::Local<v8::Object> wrapper);
   ```

2. **Implemente em `KX_V8Bindings.cpp`**:
   ```cpp
   Local<Object> KX_V8Bindings::CreateNewTypeWrapper(Isolate *isolate, CppNewType *obj)
   {
     Local<ObjectTemplate> template = ObjectTemplate::New(isolate);
     template->SetInternalFieldCount(1);
     
     // Adicionar propriedades e métodos
     template->SetAccessor(/* ... */);
     template->Set(/* métodos */);
     
     Local<Object> wrapper = template->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
     wrapper->SetInternalField(0, External::New(isolate, obj));
     wrapper->Set(String::NewFromUtf8Literal(isolate, "__bgeType"),
                 String::NewFromUtf8Literal(isolate, "NewType"));
     return wrapper;
   }
   ```

3. **Adicione interface em `BGE_DTS_CONTENT`**:
   ```cpp
   "interface BGENewType {\n"
   "  propriedade1: tipo1;\n"
   "  metodo1(): void;\n"
   "}\n"
   ```

4. **Sincronize com `text_lsp_ts.cc`**

5. **Recompile e teste**.

### Modificar Compilação TypeScript

1. **Edite `KX_TypeScriptCompiler.cpp`**:
   - Localize `CompileWithTSC()` (linha 166)
   - Modifique a linha do comando `tsc`:
     ```cpp
     std::string command = "tsc --target ES2020 --module none --strict " + temp_ts_file;
     //                                                      ^^^^^^^^^^ novas flags
     ```

2. **Recompile**: Mudanças afetam como TypeScript é compilado.

### Adicionar Constante ao `bge.events`

1. **Edite `KX_V8Bindings.cpp`**:
   - Localize `SetupBGENamespace()` (linha 132)
   - Adicione constante:
     ```cpp
     events_obj->Set(context, String::NewFromUtf8Literal(isolate, "NOVAKEY"),
                     Integer::New(isolate, SCA_IInputDevice::NOVAKEY)).Check();
     ```

2. **Atualize `BGE_DTS_CONTENT`**:
   ```cpp
   "  events: {\n"
   "    WKEY: number; SKEY: number; AKEY: number; DKEY: number;\n"
   "    NOVAKEY: number; // Adicione aqui\n"
   "    ACTIVE: number; JUSTACTIVATED?: number; JUSTRELEASED?: number;\n"
   "  };\n"
   ```

3. **Sincronize com `text_lsp_ts.cc`**

4. **Recompile e teste**.

## Sincronização de BGE_DTS_CONTENT

**CRÍTICO**: `BGE_DTS_CONTENT` existe em **dois lugares**:

1. `source/gameengine/Ketsji/KX_TypeScriptCompiler.cpp` (linha 82)
2. `source/blender/editors/space_text/text_lsp_ts.cc` (linha 37)

**Sempre mantenha ambos sincronizados!** Qualquer mudança nas definições TypeScript deve ser refletida em ambos os arquivos.

### Processo de Sincronização

1. **Modifique `KX_TypeScriptCompiler.cpp`** primeiro (fonte de verdade)
2. **Copie as mudanças** para `text_lsp_ts.cc`
3. **Verifique** que ambos estão idênticos
4. **Recompile** ambos os componentes

## Flags de Compilação

### WITH_JAVASCRIPT

Habilita suporte a JavaScript (V8 engine). Sem isso, controllers JavaScript não funcionam.

**Localização**: Definido em CMakeLists.txt

### WITH_TYPESCRIPT

Habilita suporte a TypeScript (compilação via `tsc`). Requer `WITH_JAVASCRIPT`.

**Localização**: Definido em CMakeLists.txt

## Dependências Externas

### V8 Engine

- **Biblioteca**: V8 JavaScript engine (incluída no projeto)
- **Localização**: `lib/v8/`
- **Uso**: Execução de código JavaScript

### TypeScript Compiler (tsc)

- **Ferramenta**: `tsc` (TypeScript Compiler)
- **Requisito**: Deve estar no PATH do sistema
- **Verificação**: `KX_TypeScriptCompiler::IsAvailable()` verifica via `tsc --version`
- **Uso**: Compilação de TypeScript para JavaScript

## Troubleshooting

### TypeScript Não Compila

1. **Verifique se `tsc` está disponível**:
   ```bash
   tsc --version
   ```

2. **Verifique logs de erro**:
   - `CM_Error()` em `KX_TypeScriptCompiler.cpp` mostra erros de compilação

3. **Verifique arquivos temporários**:
   - Arquivos `.ts` e `.d.ts` são criados temporariamente
   - Se houver erro, podem não ser removidos

### JavaScript Não Executa

1. **Verifique se V8 está inicializado**:
   - `KX_V8Engine::GetInstance()` deve retornar não-nulo

2. **Verifique erros de compilação V8**:
   - `CM_Error()` mostra erros de compilação JavaScript

3. **Verifique contexto V8**:
   - Cada controller tem seu próprio contexto
   - Bindings devem ser inicializados em cada contexto

### Bindings Não Funcionam

1. **Verifique se `InitializeBindings()` foi chamado**:
   - Deve ser chamado em `SCA_JavaScriptController::Compile()`

2. **Verifique se objeto existe**:
   - Use `GetGameObjectFromWrapper()` para obter objeto C++

3. **Verifique tipos de argumentos**:
   - Use `args.Length()` para verificar número de argumentos
   - Use `args[i]->IsNumber()`, `args[i]->IsArray()`, etc.

### Erros de Tipo TypeScript

1. **Verifique `BGE_DTS_CONTENT`**:
   - Definições devem corresponder aos bindings reais

2. **Sincronize `text_lsp_ts.cc`**:
   - Editor usa definições diferentes, mas devem ser iguais

3. **Verifique sintaxe TypeScript**:
   - Interfaces devem estar corretas

## Exemplos de Código

### Exemplo: Adicionar Função `bge.logic.getDeltaTime()`

**1. `KX_V8Bindings.h`**:
```cpp
static void GetDeltaTime(const v8::FunctionCallbackInfo<v8::Value> &args);
```

**2. `KX_V8Bindings.cpp`**:
```cpp
void KX_V8Bindings::GetDeltaTime(const FunctionCallbackInfo<Value> &args)
{
  Isolate *isolate = args.GetIsolate();
  extern SCA_JavaScriptController *g_currentJavaScriptController;
  if (!g_currentJavaScriptController) {
    args.GetReturnValue().SetNull();
    return;
  }
  
  KX_Scene *scene = g_currentJavaScriptController->GetScene();
  if (!scene) {
    args.GetReturnValue().SetNull();
    return;
  }
  
  double delta = scene->GetKetsjiEngine()->GetClockTime();
  args.GetReturnValue().Set(Number::New(isolate, delta));
}

// Em SetupLogicObject():
logic_template->Set(String::NewFromUtf8Literal(isolate, "getDeltaTime"),
                    FunctionTemplate::New(isolate, GetDeltaTime));
```

**3. `KX_TypeScriptCompiler.cpp` (BGE_DTS_CONTENT)**:
```cpp
"declare const bge: {\n"
"  logic: {\n"
"    getCurrentController(): BGEController | null;\n"
"    getCurrentScene(): BGEScene | null;\n"
"    getCurrentControllerObject(): BGEGameObject | null;\n"
"    getDeltaTime(): number; // Adicione aqui\n"
"  };\n"
```

**4. `text_lsp_ts.cc` (BGE_DTS_CONTENT)** - sincronize!

### Exemplo: Adicionar Propriedade `GameObject.mass`

**1. `KX_V8Bindings.h`**:
```cpp
static void GameObjectGetMass(v8::Local<v8::Name> property,
                             const v8::PropertyCallbackInfo<v8::Value> &info);
static void GameObjectSetMass(v8::Local<v8::Name> property,
                              v8::Local<v8::Value> value,
                              const v8::PropertyCallbackInfo<void> &info);
```

**2. `KX_V8Bindings.cpp`**:
```cpp
void KX_V8Bindings::GameObjectGetMass(Local<Name> property,
                                     const PropertyCallbackInfo<Value> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj) {
    return;
  }
  
  // Obter massa do objeto físico
  PHY_IPhysicsController *phys = obj->GetPhysicsController();
  if (phys) {
    double mass = phys->GetMass();
    info.GetReturnValue().Set(Number::New(isolate, mass));
  }
  else {
    info.GetReturnValue().Set(Number::New(isolate, 0.0));
  }
}

void KX_V8Bindings::GameObjectSetMass(Local<Name> property,
                                     Local<Value> value,
                                     const PropertyCallbackInfo<void> &info)
{
  Isolate *isolate = info.GetIsolate();
  Local<Object> self = info.This();
  KX_GameObject *obj = GetGameObjectFromWrapper(self);
  if (!obj) {
    return;
  }
  
  Local<Context> context = isolate->GetCurrentContext();
  double mass = value->NumberValue(context).FromMaybe(0.0);
  
  PHY_IPhysicsController *phys = obj->GetPhysicsController();
  if (phys) {
    phys->SetMass(mass);
  }
}

// Em CreateGameObjectWrapper():
template->SetAccessor(String::NewFromUtf8Literal(isolate, "mass"),
                     GameObjectGetMass,
                     GameObjectSetMass);
```

**3. `KX_TypeScriptCompiler.cpp` (BGE_DTS_CONTENT)**:
```cpp
"interface BGEGameObject {\n"
"  name: string;\n"
"  position: [number, number, number];\n"
"  mass: number; // Adicione aqui\n"
"  // ...\n"
"}\n"
```

**4. `text_lsp_ts.cc` (BGE_DTS_CONTENT)** - sincronize!

## Notas Importantes

1. **Contextos V8 são isolados**: Cada controller tem seu próprio contexto, mas compartilham o mesmo isolate
2. **Wrappers são temporários**: Wrappers JavaScript são criados a cada acesso, não são persistentes
3. **TypeScript é compilado em tempo de execução**: Compilação acontece quando o controller é compilado
4. **BGE_DTS_CONTENT deve estar sincronizado**: Sempre mantenha ambos os arquivos iguais
5. **V8 usa handles**: Sempre use `Local<>`, `Global<>`, `HandleScope` corretamente
6. **Exceções V8**: Use `isolate->ThrowException()` para lançar exceções JavaScript

## Referências

- [V8 Embedder's Guide](https://v8.dev/docs/embed)
- [TypeScript Handbook](https://www.typescriptlang.org/docs/handbook/intro.html)
- [Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
