// rt_shaders.hlsl — oclusão ambiente com raios (DXR 1.1, RayQuery inline, SM 6.5).
// Compilado offline por tools/build_rt_shaders.py (dxc do Windows SDK) para rt_shaders.inc; o gow.exe só embute o DXIL.
//
// Geometria: as posições à saída dos vertex shaders traduzidos (stream-out de SV_Position: x, y e w de recorte) dos desenhos opacos
// da cena. Para a projeção do jogo w = z de vista, por isso (x_c, y_c, w) é o espaço de vista com x e y escalados por 1/tan(FOV/2):
// uma transformação linear, que não muda as interseções. A câmara está na origem; o raio primário de cada píxel é (ndc.x, ndc.y, 1)
// e acerta na superfície à distância w. Para as direções da AO multiplica-se x e y por tan(FOV/2) (FOV assumido, como nos efeitos
// de ecrã): só muda a forma do hemisfério, não as interseções.
cbuffer C : register(b0) {
  float4 c0;  // origem da região no RT (px internos), tamanho da região
  float4 c1;  // origem da região no ecrã, tamanho do ecrã (px internos)
  float4 c2;  // tan(FOV/2) x/y, fator de redução (1 ou 2), raio da AO (unidades do jogo)
  float4 c3;  // intensidade, nº de raios, -, -
};
RaytracingAccelerationStructure scene : register(t0);
ByteAddressBuffer verts : register(t1);  // float3 por vértice, 3 por triângulo

struct VSOut { float4 pos : SV_Position; };
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}

uint hash(uint x) {
  x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
  return x;
}

float4 ps_main(VSOut i) : SV_Target {
  const float2 p = (i.pos.xy - 0.5) * c2.z + 0.5 * (c2.z - 1.0);  // píxel na região, resolução inteira
  const float2 s = (p + 0.5 + c1.xy) / c1.zw;
  const float2 ndc = float2(s.x * 2.0 - 1.0, 1.0 - s.y * 2.0);
  RayDesc r;
  r.Origin = float3(0, 0, 0);
  r.Direction = float3(ndc, 1.0);
  r.TMin = 0.5;
  r.TMax = 1e7;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
  q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r);
  q.Proceed();
  if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return float4(0, 0, 0, 1);
  const float t = q.CommittedRayT();
  const uint prim = q.CommittedPrimitiveIndex();
  const float3 sc = float3(c2.x, c2.y, 1.0);  // espaço do BLAS -> vista isotrópica
  const float3 A = asfloat(verts.Load3(prim * 36)) * sc, B = asfloat(verts.Load3(prim * 36 + 12)) * sc,
               Cc = asfloat(verts.Load3(prim * 36 + 24)) * sc;
  const float3 P = float3(ndc * t, t) * sc;
  float3 N = cross(B - A, Cc - A);
  const float nl = length(N);
  if (!(nl > 0.0)) return float4(0, 0, 0, 1);
  N /= nl;
  if (dot(N, -P) < 0.0) N = -N;
  const float3 T = normalize(abs(N.y) < 0.99 ? cross(N, float3(0, 1, 0)) : cross(N, float3(1, 0, 0)));
  const float3 Bt = cross(N, T);
  const float R = c2.w;
  const uint rays = (uint)c3.y;
  uint seed = hash((uint)i.pos.x * 1973u + (uint)i.pos.y * 9277u);
  float occ = 0.0;
  const float3 origin = (P + N * (0.002 * t + 0.5)) / sc;
  for (uint k = 0; k < rays; k++) {
    seed = hash(seed + k);
    const float u1 = (seed & 0xFFFF) / 65536.0, u2 = (seed >> 16) / 65536.0;
    const float rr = sqrt(u1), phi = 6.2831853 * ((k + u2) / rays);  // cosseno, com as direções espalhadas por k
    const float3 d = T * (rr * cos(phi)) + Bt * (rr * sin(phi)) + N * sqrt(max(0.0, 1.0 - u1));
    RayDesc ar;
    ar.Origin = origin;
    ar.Direction = d / sc;  // |d| = 1 na vista: t é a distância real
    ar.TMin = 0.0;
    ar.TMax = R;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> aq;
    aq.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ar);
    aq.Proceed();
    if (aq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
      const float f = aq.CommittedRayT() / R;
      occ += 1.0 - f * f;
    }
  }
  const float ao = 1.0 - occ / max(rays, 1u);
  return float4(0, 0, 0, lerp(1.0, ao, c3.x));
}
