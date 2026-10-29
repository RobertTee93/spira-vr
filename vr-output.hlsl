cbuffer Params : register(b0) {
 float4 bounds; // left, right, top, bottom, in headset tangent space
 float4 projection; // source half-tangents, depth A/W, depth B/W
 float4 settings; // signed eye offset in game units, maximum UV shift, world present, depth present
 float4 panel; // width, height, distance, signed eye offset in metres
 float4 rot0; float4 rot1; float4 rot2;
 float4 outputInfo; // width, height, desktop mode, UI enabled
 float4 warp0; float4 warp1; float4 warp2;
};
Texture2D<float4> scene : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float4> ui : register(t2);
SamplerState linearClamp : register(s0);
SamplerState pointClamp : register(s1);
RWTexture2D<float4> outputImage : register(u0);
float3 rotateHead(float3 v) {return float3(dot(rot0.xyz,v),dot(rot1.xyz,v),dot(rot2.xyz,v));}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy >= uint2(outputInfo.xy)))return;
 float2 uv=(id.xy+.5)/outputInfo.xy;
 if(outputInfo.z>.5) {
   float4 c=scene.SampleLevel(linearClamp,uv,0);
   float4 h=ui.SampleLevel(linearClamp,uv,0);
   outputImage[id.xy]=float4(h.rgb+c.rgb*(1-saturate(h.a)),1);return;
 }
 float2 ray=float2(lerp(bounds.x,bounds.y,uv.x),lerp(bounds.z,bounds.w,uv.y));
 float3 colour=float3(.008,.009,.012);
 if(settings.z>.5) {
   // Rotate the latest display ray into the actual render pose, including
   // DLSS5's previous-frame delivery. UI uses the latest pose independently.
   float3 v=float3(ray.x,-ray.y,-1);
   float3 sourceRay=float3(dot(warp0.xyz,v),dot(warp1.xyz,v),dot(warp2.xyz,v));
   float2 projected=float2(sourceRay.x,-sourceRay.y)/max(.001,-sourceRay.z);
   float2 source=.5+projected/(2*projection.xy)+float2(rot0.w,-rot1.w)*.5, sampleUv=source;
   if(settings.w>.5) {
     // Final conservative mode: a continuous, bounded displacement avoids
     // fixed-point cycling and duplicate silhouettes at depth discontinuities.
     // This intentionally trades strong object depth for clean model edges.
     uint depthWidth,depthHeight;depth.GetDimensions(depthWidth,depthHeight);
     float texel=1.0/depthWidth;
     float limit=min(settings.y,4*texel),shift=0;
     [unroll]for(int j=-4;j<=4;j++) {
       float z=depth.SampleLevel(linearClamp,source+float2(j*2*texel,0),0);
       float invZ=abs((z-projection.z)/projection.w);
       shift+=(5-abs(j))*clamp(settings.x*invZ/(2*projection.x),-limit,limit);
     }
     sampleUv=source+float2(shift/25.0,0);
   }
   if(sourceRay.z<0&&all(sampleUv>=0)&&all(sampleUv<=1))colour=scene.SampleLevel(linearClamp,sampleUv,0).rgb;
 }
 if(outputInfo.w>.5) {
   float3 direction=rotateHead(float3(ray.x,-ray.y,-1));
   float3 origin=rotateHead(float3(panel.w,0,0));
   float t=(-panel.z-origin.z)/direction.z;
   float3 hit=origin+t*direction;
   float2 hudUv=float2(hit.x/panel.x+.5,.5-hit.y/panel.y);
   if(t>0&&all(hudUv>=0)&&all(hudUv<=1)) {
     float4 h=ui.SampleLevel(linearClamp,hudUv,0);
     if(rot2.w>.5)h.a=1;
     colour=h.rgb+colour*(1-saturate(h.a));
   }
 }
 outputImage[id.xy]=float4(colour,1);
}
