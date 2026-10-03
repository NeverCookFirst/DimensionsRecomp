// Xenos RB_COLORCONTROL alpha_func. Test original shader alpha before EDRAM
// exponent scaling. Ordered comparisons reject NaNs; NOTEQUAL accepts them.
bool LegoAlphaPass(float alpha, float reference, uint function)
{
    switch (function & 7u)
    {
        case 0u: return false;
        case 1u: return alpha < reference;
        case 2u: return alpha == reference;
        case 3u: return alpha <= reference;
        case 4u: return alpha > reference;
        case 5u: return alpha != reference;
        case 6u: return alpha >= reference;
        default: return true;
    }
}
