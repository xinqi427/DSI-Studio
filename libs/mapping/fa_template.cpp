#include "image/image.hpp"
#include "fa_template.hpp"
#include "libs/gzip_interface.hpp"

#include <QApplication>
#include <QDir>

extern std::string fa_template_file_name,fa_mask_file_name;
bool fa_template::load_from_file(void)
{
    gz_nifti read;
    if(read.load_from_file(fa_template_file_name.c_str()))
    {
        read.toLPS(I);
        read.get_image_transformation(tran);
        tran[12] = tran[13] = tran[14] = 0.0f;
        tran[15] = 1.0f;
        vs[0] = tran[0];
        vs[1] = tran[5];
        vs[2] = tran[10];
        vs.abs();
        shift[0] = tran[3];
        shift[1] = tran[7];
        shift[2] = tran[11];
        gz_nifti read_mask;
        if(read_mask.load_from_file(fa_mask_file_name.c_str()))
        {
            read_mask.toLPS(mask);
            return mask.geometry() == I.geometry();
        }
        error_msg = "Failed to load mask file at ";
        error_msg += fa_mask_file_name;
        return false;
    }
    error_msg = "Failed to load template file at ";
    error_msg += fa_template_file_name;
    return false;
}




