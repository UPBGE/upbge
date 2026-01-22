# Manutenção do Suporte TypeScript/JavaScript

Este documento explica como dar manutenção no suporte a TypeScript e JavaScript no editor de texto do UPBGE.

## Visão Geral

O suporte a TypeScript/JavaScript no editor consiste em três componentes principais:

1. **Syntax Highlighting** (`text_format_js.cc`) - Coloração de sintaxe baseada em análise estática
2. **Autocompletar via LSP** (`text_lsp_ts.cc`) - Integração com `typescript-language-server` para sugestões inteligentes
3. **Sistema de Cores** (`text_draw.cc`) - Aplicação das cores no tema One Dark

## Arquitetura

### Fluxo de Dados

```
Arquivo .js/.ts
    ↓
text_format_js.cc (txtfmt_js_format_line)
    ↓
Array de formatos (FMT_TYPE_*)
    ↓
text_draw.cc (format_draw_color)
    ↓
Cores One Dark aplicadas
```

### Autocompletar

```
Ctrl+Space
    ↓
text_autocomplete.cc
    ↓
ts_lsp_get_completions (text_lsp_ts.cc)
    ↓
typescript-language-server (via stdio)
    ↓
Filtro de sugestões
    ↓
texttool_suggest_add
```

## Componentes Principais

### 1. Syntax Highlighting (`text_format_js.cc`)

**Localização:** `source/blender/editors/space_text/text_format_js.cc`

#### Estrutura

O arquivo contém:

- **Arrays de literais** (linhas 35-120):
  - `text_format_js_literals_keyword_data[]` - Palavras-chave JavaScript/TypeScript
  - `text_format_js_literals_value_data[]` - Valores literais (true, false, null, undefined)
  - `text_format_js_literals_type_data[]` - Tipos primitivos (string, number, boolean, etc.)

- **Funções de busca** (linhas 128-153):
  - `txtfmt_js_find_keyword()` - Encontra palavras-chave
  - `txtfmt_js_find_value()` - Encontra valores literais
  - `txtfmt_js_find_type()` - Encontra tipos primitivos

- **Função principal de formatação** (linhas 175-373):
  - `txtfmt_js_format_line()` - Processa cada linha e atribui tipos de formato

- **Registro** (linhas 381-398):
  - `ED_text_format_register_js()` - Registra o formatador para extensões `.js`, `.mjs`, `.cjs`, `.ts`, `.mts`, `.cts`

#### Tipos de Formato (FMT_TYPE_*)

| Tipo | Cor (One Dark) | Uso |
|------|----------------|-----|
| `FMT_TYPE_DEFAULT` | Branco (#ABB2BF) | Identificadores padrão, variáveis |
| `FMT_TYPE_KEYWORD` | Roxo (#C678DD) | Palavras-chave (const, if, function, etc.) |
| `FMT_TYPE_RESERVED` | Roxo (#C678DD) | Tipos primitivos (string, number, boolean) |
| `FMT_TYPE_NUMERAL` | Laranja (#D19A66) | Números e valores literais (true, false, null) |
| `FMT_TYPE_STRING` | Verde (#98C379) | Strings entre aspas |
| `FMT_TYPE_COMMENT` | Cinza (#5C6370) | Comentários |
| `FMT_TYPE_DIRECTIVE` | Amarelo (#E5C07B) | Nomes de tipos/interfaces (após `interface`, `class`, `type`, `as`) |
| `FMT_TYPE_SPECIAL` | Azul/Vermelho | Propriedades (vermelho) e funções (azul) após ponto |
| `FMT_TYPE_SYMBOL` | Branco (#ABB2BF) | Símbolos e pontuação |
| `FMT_TYPE_WHITESPACE` | - | Espaços em branco |

#### Lógica de Formatação

A função `txtfmt_js_format_line()` processa caracteres em sequência:

1. **Escape sequences** - Ignora `\` e próximo caractere
2. **Continuations** - Lida com strings/comentários multi-linha
3. **Comentários** - `//` (linha) ou `/* */` (bloco)
4. **Strings** - `"` ou `'`
5. **Números** - Dígitos e pontos decimais
6. **Delimitadores** - Detecta ponto (`.`) para propriedades
7. **Identificadores** - Verifica se é palavra-chave, tipo, valor ou identificador padrão

#### Variáveis de Estado

- `after_dot` - Indica se estamos após um ponto (para detectar propriedades)
- `expect_type_name` - Indica se esperamos um nome de tipo (após `interface`, `class`, `type`)
- `after_as` - Indica se estamos após `as` (para type assertions)

### 2. Autocompletar via LSP (`text_lsp_ts.cc`)

**Localização:** `source/blender/editors/space_text/text_lsp_ts.cc`

#### Estrutura

- **BGE Type Definitions** (linhas 37-131):
  - `BGE_DTS_CONTENT` - Definições TypeScript dos tipos BGE (interfaces, tipos, globais)
  - Inclui: `BGEGameObject`, `BGEScene`, `BGEController`, `BGEVehicle`, `BGECharacter`, `bge` global

- **Estado da Sessão LSP** (linhas 139-145):
  - `ts_lsp_pipe` - Pipe de comunicação com o servidor
  - `ts_lsp_inited` - Flag de inicialização
  - `ts_lsp_uri` - URI do documento atual
  - `ts_lsp_version` - Versão do documento (incrementa a cada mudança)

- **Funções JSON-RPC** (linhas 153-230):
  - `lsp_send()` - Envia mensagens JSON-RPC
  - `lsp_read_message()` - Lê mensagens JSON-RPC (formato: `Content-Length: N\r\n\r\n{body}`)

- **Ciclo de Vida LSP** (linhas 238-337):
  - `ts_lsp_ensure_started()` - Inicia o servidor via `npx typescript-language-server --stdio`
  - `ts_lsp_ensure_document()` - Mantém o documento sincronizado (didOpen/didChange/didClose)

- **API Pública** (linhas 345-489):
  - `text_format_is_js_or_ts()` - Verifica se o formatador é JS/TS
  - `ts_lsp_get_completions()` - Obtém sugestões do LSP
  - `ts_lsp_shutdown()` - Encerra o servidor

#### Fluxo de Autocompletar

1. Usuário pressiona `Ctrl+Space`
2. `text_autocomplete.cc` chama `ts_lsp_get_completions()`
3. Função prepara o documento completo: `BGE_DTS_CONTENT + "\n" + conteúdo_do_arquivo`
4. Calcula posição do cursor (ajustando para offset do BGE_DTS)
5. Envia `textDocument/completion` ao LSP
6. Recebe resposta com array de `CompletionItem`
7. **Filtra sugestões** (ver seção abaixo)
8. Adiciona sugestões via `texttool_suggest_add()`

#### Filtro de Sugestões

O filtro atual (se implementado) deve:

- **Após ponto (`.`)**: Mostrar apenas `Property` (kind 5) e `Method` (kind 2)
- **Filtrar inválidos**: Caracteres únicos não-alfanuméricos, comentários, palavras-chave
- **Priorizar**: Property > Method > Variable/Function > Class/Interface

**LSP CompletionItemKind:**
- `2` = Method
- `3` = Function
- `5` = Property
- `6` = Variable
- `7` = Class
- `8` = Interface

### 3. Sistema de Cores (`text_draw.cc`)

**Localização:** `source/blender/editors/space_text/text_draw.cc`

#### Função Principal

`format_draw_color()` (linhas 165-266) aplica cores baseadas no tipo de formato.

#### Tema One Dark

Cores definidas (linhas 140-157):

```cpp
one_dark_foreground  = #ABB2BF (branco)
one_dark_comment     = #5C6370 (cinza)
one_dark_string      = #98C379 (verde)
one_dark_keyword     = #C678DD (roxo)
one_dark_numeral     = #D19A66 (laranja)
one_dark_directive   = #E5C07B (amarelo)
one_dark_special     = #61AFEF (azul - funções)
one_dark_reserved    = #C678DD (roxo - tipos primitivos)
one_dark_symbol      = #ABB2BF (branco)
one_dark_variable    = #E06C75 (vermelho - propriedades)
```

#### Lógica Especial para FMT_TYPE_SPECIAL

Quando `formatchar == FMT_TYPE_SPECIAL`:

1. Verifica se o caractere anterior é `FMT_TYPE_SYMBOL` (provavelmente `.`)
2. Se sim, usa `one_dark_variable` (vermelho) - **propriedade**
3. Se não, usa `one_dark_special` (azul) - **função**

## Guias de Manutenção

### Adicionar Nova Palavra-Chave

1. **Edite `text_format_js.cc`**:
   - Adicione a palavra-chave em `text_format_js_literals_keyword_data[]` (linha 35)
   - **IMPORTANTE**: Mantenha o array **ordenado alfabeticamente** (necessário para busca binária)
   - Exemplo:
     ```cpp
     static const char *text_format_js_literals_keyword_data[] = {
         /* clang-format off */
         "as",
         "async",
         "await",
         "break",
         // ... sua nova palavra-chave aqui (em ordem alfabética)
         /* clang-format on */
     };
     ```

2. **Recompile**:
   ```bash
   make
   ```

3. **Teste**: A palavra-chave deve aparecer em roxo no editor.

### Adicionar Novo Tipo Primitivo

1. **Edite `text_format_js.cc`**:
   - Adicione em `text_format_js_literals_type_data[]` (linha 104)
   - Mantenha ordenado alfabeticamente
   - Exemplo:
     ```cpp
     static const char *text_format_js_literals_type_data[] = {
         /* clang-format off */
         "any",
         "bigint",
         "boolean",
         // ... seu novo tipo aqui
         /* clang-format on */
     };
     ```

2. **Recompile e teste**: O tipo deve aparecer em roxo.

### Modificar Cores

1. **Edite `text_draw.cc`**:
   - Localize as definições de cores One Dark (linhas 140-157)
   - Modifique o valor RGB desejado
   - Exemplo para mudar cor de keywords:
     ```cpp
     static const unsigned char one_dark_keyword[4] = {0xFF, 0x00, 0x00, 255}; // Vermelho
     ```

2. **Recompile e teste**.

### Ajustar Filtro de Autocompletar

1. **Edite `text_lsp_ts.cc`**:
   - Localize a função `ts_lsp_get_completions()` (linha 361)
   - Encontre o loop que processa `items` (linha 449)
   - Adicione/remova filtros conforme necessário

2. **Exemplo de filtro**:
   ```cpp
   for (const nlohmann::json &it : items) {
       std::string label = it.value("insertText", it.value("label", ""));
       
       // Filtrar sugestões vazias
       if (label.empty()) {
           continue;
       }
       
       // Filtrar palavras-chave
       if (label == "const" || label == "let" || label == "var") {
           continue;
       }
       
       // Filtrar por tipo (kind)
       int kind = it.value("kind", 0);
       if (kind == 1) { // Text - pular
           continue;
       }
       
       // Adicionar sugestão
       char type = tft->format_identifier(label.c_str());
       texttool_suggest_add(label.c_str(), type);
   }
   ```

3. **Recompile e teste**.

### Adicionar/Modificar Tipos BGE no LSP

1. **Edite `text_lsp_ts.cc`**:
   - Localize `BGE_DTS_CONTENT` (linha 37)
   - Adicione/modifique interfaces TypeScript conforme necessário
   - Exemplo para adicionar nova propriedade:
     ```cpp
     static const char *BGE_DTS_CONTENT =
         "interface BGEGameObject {\n"
         "  name: string;\n"
         "  position: [number, number, number];\n"
         "  novaPropriedade: string; // Adicione aqui\n"
         // ...
     ```

2. **Recompile**: O LSP usará as novas definições na próxima inicialização.

### Modificar Comportamento de Syntax Highlighting

#### Exemplo: Fazer `as` ser branco em vez de roxo

1. **Edite `text_format_js.cc`**:
   - Localize a função `txtfmt_js_format_line()` (linha 175)
   - Encontre onde palavras-chave são processadas (linha 291)
   - Adicione tratamento especial:
     ```cpp
     else if ((i = txtfmt_js_find_keyword(str)) != -1) {
         /* Special handling for 'as' - should be white (default), not purple */
         if (i == 2 && strncmp(str, "as", 2) == 0) {
             ident_type = FMT_TYPE_DEFAULT; /* 'as' should be white */
             after_as = true; /* Next identifier after 'as' is a type name */
         }
         else {
             ident_type = FMT_TYPE_KEYWORD;
         }
     }
     ```

2. **Recompile e teste**.

#### Exemplo: Colorir tipo após `as` em amarelo

1. **Adicione flag `after_as`** (se não existir):
   ```cpp
   bool after_as = false; /* Track if we're after 'as' keyword */
   ```

2. **No processamento de identificadores**:
   ```cpp
   else if (expect_type_name || after_as) {
       /* This is a type name after interface/class/type/as */
       ident_type = FMT_TYPE_DIRECTIVE; /* Type/interface name: yellow */
       // ... calcular comprimento do identificador
       expect_type_name = false;
       after_as = false;
   }
   ```

3. **Recompile e teste**.

### Adicionar Suporte a Nova Extensão

1. **Edite `text_format_js.cc`**:
   - Localize `ED_text_format_register_js()` (linha 381)
   - Adicione a extensão no array `ext[]`:
     ```cpp
     static const char *ext[] = {"js", "mjs", "cjs", "ts", "mts", "cts", "jsx", nullptr};
     //                                                                      ^^^^ nova extensão
     ```

2. **Recompile**: Arquivos com a nova extensão terão syntax highlighting.

## Troubleshooting

### Syntax Highlighting Não Funciona

1. **Verifique se o formatador está registrado**:
   - Confirme que `ED_text_format_register_js()` é chamado em `space_text.cc` (linha 497)

2. **Verifique a extensão do arquivo**:
   - Deve ser uma das: `.js`, `.mjs`, `.cjs`, `.ts`, `.mts`, `.cts`

3. **Verifique se há erros de compilação**:
   - Arrays de literais devem estar ordenados alfabeticamente
   - Verifique logs de build

### Autocompletar Não Funciona

1. **Verifique se `npx` está disponível**:
   ```bash
   npx --version
   ```

2. **Verifique se `typescript-language-server` está instalado**:
   ```bash
   npx typescript-language-server --version
   ```

3. **Verifique logs**:
   - O servidor LSP é iniciado via `npx typescript-language-server --stdio`
   - Se falhar, verifique se Node.js está instalado

4. **Verifique se a função é chamada**:
   - Confirme que `ts_lsp_get_completions()` é chamado em `text_autocomplete.cc`

### Cores Estão Incorretas

1. **Verifique o tipo de formato atribuído**:
   - Adicione logs em `txtfmt_js_format_line()` para ver qual `FMT_TYPE_*` está sendo usado

2. **Verifique `format_draw_color()`**:
   - Confirme que o switch case está correto para o tipo de formato

3. **Verifique se `use_onedark` está habilitado**:
   - O tema One Dark só é aplicado se `tdc->use_onedark == true`

### Sugestões de Autocompletar Estão Incorretas

1. **Verifique o filtro**:
   - Adicione logs para ver quais sugestões estão sendo recebidas do LSP
   - Verifique se o filtro está removendo sugestões corretas

2. **Verifique `BGE_DTS_CONTENT`**:
   - Confirme que as definições TypeScript estão corretas
   - O LSP usa essas definições para inferir tipos

3. **Verifique posição do cursor**:
   - O offset de linha deve incluir o número de linhas em `BGE_DTS_CONTENT`

## Estrutura de Arquivos

```
source/blender/editors/space_text/
├── text_format_js.cc          # Syntax highlighting JS/TS
├── text_lsp_ts.cc              # Integração LSP para autocompletar
├── text_lsp_ts.h               # Header do LSP
├── text_draw.cc                # Sistema de cores e desenho
├── text_autocomplete.cc         # Sistema de autocompletar (chama LSP)
├── text_format.hh               # Definições de TextFormatType
└── space_text.cc                # Registro dos formatadores
```

## Dependências

- **Node.js/npx**: Necessário para executar `typescript-language-server`
- **typescript-language-server**: Instalado via `npx` automaticamente
- **nlohmann/json**: Biblioteca JSON para comunicação LSP (já incluída no projeto)

## Notas Importantes

1. **Arrays de literais devem estar ordenados**: `text_format_string_literal_find()` usa busca binária
2. **UTF-8**: O código lida com UTF-8, use `BLI_str_utf8_size_safe()` para caracteres multi-byte
3. **LSP é assíncrono**: O servidor pode demorar para processar mudanças no documento
4. **BGE_DTS_CONTENT é pré-pendido**: Todas as definições BGE são adicionadas antes do código do usuário
5. **One Dark é opcional**: O sistema funciona sem o tema One Dark, usando cores do tema padrão

## Referências

- [Language Server Protocol Specification](https://microsoft.github.io/language-server-protocol/)
- [TypeScript Language Server](https://github.com/typescript-language-server/typescript-language-server)
- [One Dark Theme Colors](https://github.com/atom/one-dark-syntax)
