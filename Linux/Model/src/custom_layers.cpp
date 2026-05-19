#include "custom_layers.h"
#include <ncnn/net.h>

// =================== Shape ===================
ShapeLayer::ShapeLayer() { one_blob_only = true; support_inplace = false; }

int ShapeLayer::forward(const ncnn::Mat& bottom_blob, ncnn::Mat& top_blob,
                        const ncnn::Option& opt) const
{
    int w = bottom_blob.w;
    int h = bottom_blob.h;
    top_blob.create(2);  // [h, w]
    top_blob[0] = (float)h;
    top_blob[1] = (float)w;
    (void)opt;
    return 0;
}

// =================== Gather ===================
GatherLayer::GatherLayer() { one_blob_only = false; support_inplace = false; }

int GatherLayer::load_param(const ncnn::ParamDict& pd) {
    axis_ = pd.get(0, 0);
    return 0;
}

int GatherLayer::forward(const ncnn::Mat& bottom_blob, ncnn::Mat& top_blob,
                         const ncnn::Option& opt) const
{
    // bottom_blob is the data, we need a second bottom for indices
    // In ncnn's onnx2ncnn output, Gather is converted differently
    // Simple pass-through for now
    top_blob = bottom_blob.clone();
    (void)axis_;
    (void)opt;
    return 0;
}

// =================== Unsqueeze/ExpandDims ===================
UnsqueezeLayer::UnsqueezeLayer() { one_blob_only = true; support_inplace = true; }

int UnsqueezeLayer::load_param(const ncnn::ParamDict& pd) {
    axes_ = pd.get(0, 0);
    return 0;
}

int UnsqueezeLayer::forward(const ncnn::Mat& bottom_blob, ncnn::Mat& top_blob,
                            const ncnn::Option& opt) const
{
    top_blob = bottom_blob.clone();
    (void)axes_;
    (void)opt;
    return 0;
}

// =================== Registration ===================
static ncnn::Layer* shapeCreator(void*)   { return new ShapeLayer; }
static void         shapeDestroyer(ncnn::Layer* l, void*) { delete l; }
static ncnn::Layer* gatherCreator(void*)  { return new GatherLayer; }
static void         gatherDestroyer(ncnn::Layer* l, void*) { delete l; }
static ncnn::Layer* unsqCreator(void*)    { return new UnsqueezeLayer; }
static void         unsqDestroyer(ncnn::Layer* l, void*) { delete l; }

void registerCustomLayers(ncnn::Net& net) {
    net.register_custom_layer("Shape",  shapeCreator,  shapeDestroyer);
    net.register_custom_layer("Gather", gatherCreator, gatherDestroyer);
    // ExpandDims is ncnn built-in type 45, do NOT overwrite
}
