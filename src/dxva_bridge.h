#ifndef UU_DXVA_BRIDGE_H
#define UU_DXVA_BRIDGE_H

#include <d3d11.h>

HRESULT dxva_bridge_install(ID3D11Device *device, ID3D11DeviceContext *context);

#endif
