#pragma once

#include <ncnn/layer.h>

// Shape layer: computes shape of input tensor
// Used by SCRFD's dynamic shape handling
class ShapeLayer : public ncnn::Layer
{
public:
    ShapeLayer();
    virtual int forward(const ncnn::Mat& bottom_blob, ncnn::Mat& top_blob,
                        const ncnn::Option& opt) const override;
};

// Gather layer: gathers elements from input along an axis
class GatherLayer : public ncnn::Layer
{
public:
    GatherLayer();
    virtual int load_param(const ncnn::ParamDict& pd) override;
    virtual int forward(const ncnn::Mat& bottom_blob, ncnn::Mat& top_blob,
                        const ncnn::Option& opt) const override;
private:
    int axis_;
};

// Unsqueeze layer: adds a dimension of size 1
class UnsqueezeLayer : public ncnn::Layer
{
public:
    UnsqueezeLayer();
    virtual int load_param(const ncnn::ParamDict& pd) override;
    virtual int forward(const ncnn::Mat& bottom_blob, ncnn::Mat& top_blob,
                        const ncnn::Option& opt) const override;
private:
    int axes_;
};

// Register all custom layers to a ncnn::Net
void registerCustomLayers(ncnn::Net& net);
