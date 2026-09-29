void main(in uint vertex_id : SV_VertexID, out float4 position : SV_Position,
          out float2 tex_coord : TEXCOORD) {
  tex_coord = float2((vertex_id << 1) & 2, vertex_id & 2);
  position = float4(tex_coord * float2(2.0, -2.0) + float2(-1.0, 1.0),
                    0.0, 1.0);
}
