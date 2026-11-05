# SGK Grass v0.8 (experimental)

Grama mais distante no Campo de Hyrule (media ate 4800 unidades), aumento controlado da quantidade proxima, transicoes de LOD mais suaves, pre-calculo de superficies gramadas durante o fade-in e descarte do que estiver fora da visao. O modo de transicao chama SGKGrass_PrewarmTransition em Play_Update apos Environment_Update, antes de Play_Draw.

Configurar (CVar): SGK.GrassQuality=0/1/2, SGK.GrassDistance=100, SGK.GrassDensity=100, SGK.GrassFadePrewarm=1, SGK.GrassFadeWarmBudget=360 (60-480, reduzir se travar no fade), SGK.GrassRaycastBudget=300, SGK.GrassWindStrength=100, SGK.GrassEdgeCheck=1.

A mascara por colisao nao garante distinguir toda trilha de terra caso o mesmo triangulo de colisao esteja marcado como grama. Compilacao e testes no jogo ainda obrigatorios. Preserve o executavel v0.7 como backup.
