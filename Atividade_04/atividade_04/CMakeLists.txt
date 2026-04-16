# Para mais informações sobre o sistema de build, veja:
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# As cinco linhas a seguir de boilerplate devem estar no CMakeLists do seu projeto
# nesta ordem exata para que o cmake funcione corretamente
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "component")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(atividade_04)