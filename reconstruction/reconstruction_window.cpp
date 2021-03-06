#include <QSplitter>
#include <QThread>
#include "reconstruction_window.h"
#include "ui_reconstruction_window.h"
#include "dsi_interface_static_link.h"
#include "mapping/fa_template.hpp"
#include "image/image.hpp"
#include "mainwindow.h"
#include <QImage>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QSettings>
#include "prog_interface_static_link.h"
#include "tracking/region/Regions.h"
#include "libs/dsi/image_model.hpp"
#include "gzip_interface.hpp"
#include "manual_alignment.h"

void show_view(QGraphicsScene& scene,QImage I);
bool reconstruction_window::load_src(int index)
{
    begin_prog("load src");
    check_prog(index,filenames.size());
    handle.reset(new ImageModel);
    if (!handle->load_from_file(filenames[index].toLocal8Bit().begin()))
    {
        QMessageBox::information(this,"error",QString("Cannot open ") +
            filenames[index] + " : " +handle->error_msg.c_str(),0);
        check_prog(0,0);
        return false;
    }
    float m = (float)*std::max_element(handle->dwi_data[0],handle->dwi_data[0]+handle->voxel.dim.size());
    float otsu = image::segmentation::otsu_threshold(image::make_image(handle->dwi_data[0],handle->voxel.dim));
    ui->max_value->setMaximum(m*1.5f);
    ui->max_value->setMinimum(0.0f);
    ui->max_value->setSingleStep(m*0.05f);
    ui->max_value->setValue(otsu*3.0f);
    ui->min_value->setMaximum(m*1.5f);
    ui->min_value->setMinimum(0.0f);
    ui->min_value->setSingleStep(m*0.05f);
    ui->min_value->setValue(0.0f);
    update_image();
    return true;
}

void calculate_shell(const std::vector<float>& bvalues,std::vector<unsigned int>& shell);
bool is_dsi_half_sphere(const std::vector<unsigned int>& shell);
bool is_dsi(const std::vector<unsigned int>& shell);
bool is_multishell(const std::vector<unsigned int>& shell);

reconstruction_window::reconstruction_window(QStringList filenames_,QWidget *parent) :
    QMainWindow(parent),filenames(filenames_),ui(new Ui::reconstruction_window)
{
    ui->setupUi(this);
    if(!load_src(0))
        throw std::runtime_error("Cannot load src file");
    ui->ThreadCount->setMaximum(std::thread::hardware_concurrency());
    ui->toolBox->setCurrentIndex(1);
    ui->graphicsView->setScene(&scene);
    ui->view_source->setScene(&source);
    ui->b_table->setColumnWidth(0,60);
    ui->b_table->setColumnWidth(1,80);
    ui->b_table->setColumnWidth(2,80);
    ui->b_table->setColumnWidth(3,80);
    ui->b_table->setHorizontalHeaderLabels(QStringList() << "b value" << "bx" << "by" << "bz");
    ui->gqi_spectral->hide();
    ui->ODFSharpening->hide();
    v2c.two_color(image::rgb_color(0,0,0),image::rgb_color(255,255,255));
    update_dimension();

    absolute_path = QFileInfo(filenames[0]).absolutePath();


    switch(settings.value("rec_method_id",4).toInt())
    {
    case 0:
        ui->DSI->setChecked(true);
        on_DSI_toggled(true);
        break;
    case 1:
        ui->DTI->setChecked(true);
        on_DTI_toggled(true);
        break;
    case 3:
        ui->QBI->setChecked(true);
        on_QBI_toggled(true);
        break;
    case 7:
        ui->QDif->setChecked(true);
        on_QDif_toggled(true);
        break;
    default:
        ui->GQI->setChecked(true);
        on_GQI_toggled(true);
        break;
    }
    ui->AdvancedWidget->setVisible(false);
    ui->ThreadCount->setValue(settings.value("rec_thread_num",2).toInt());
    ui->NumOfFibers->setValue(settings.value("rec_num_fiber",5).toInt());
    ui->ODFDef->setCurrentIndex(settings.value("rec_gqi_def",0).toInt());
    ui->reg_method->setCurrentIndex(settings.value("rec_reg_method",0).toInt());

    ui->diffusion_sampling->setValue(settings.value("rec_gqi_sampling",1.25).toDouble());
    ui->csf_calibration->setChecked(settings.value("csf_calibration",0).toInt());
    ui->regularization_param->setValue(settings.value("rec_qbi_reg",0.006).toDouble());
    ui->SHOrder->setValue(settings.value("rec_qbi_sh_order",8).toInt());
    ui->hamming_filter->setValue(settings.value("rec_hamming_filter",17).toDouble());

    ui->odf_sharpening->setCurrentIndex(settings.value("rec_odf_sharpening",0).toInt());
    ui->decon_param->setValue(settings.value("rec_deconvolution_param",3.0).toDouble());
    ui->decom_m->setValue(settings.value("rec_decom_m",10).toInt());

    ui->resolution->setCurrentIndex(settings.value("rec_resolution",2).toInt());

    ui->ODFDim->setCurrentIndex(settings.value("odf_order",3).toInt());

    ui->RecordODF->setChecked(settings.value("rec_record_odf",0).toInt());
    ui->output_jacobian->setChecked(settings.value("output_jacobian",0).toInt());
    ui->output_mapping->setChecked(settings.value("output_mapping",0).toInt());
    ui->output_diffusivity->setChecked(settings.value("output_diffusivity",1).toInt());
    ui->output_tensor->setChecked(settings.value("output_tensor",0).toInt());
    ui->rdi->setChecked(settings.value("output_rdi",1).toInt());
    ui->check_btable->setChecked(settings.value("check_btable",1).toInt());

    ui->report->setText(handle->voxel.report.c_str());

    max_source_value = *std::max_element(handle->dwi_data.back(),
                                         handle->dwi_data.back()+handle->voxel.dim.size());



    on_odf_sharpening_currentIndexChanged(ui->odf_sharpening->currentIndex());
    connect(ui->z_pos,SIGNAL(valueChanged(int)),this,SLOT(on_b_table_itemSelectionChanged()));
    connect(ui->max_value,SIGNAL(valueChanged(double)),this,SLOT(on_b_table_itemSelectionChanged()));
    connect(ui->min_value,SIGNAL(valueChanged(double)),this,SLOT(on_b_table_itemSelectionChanged()));

    on_b_table_itemSelectionChanged();


    {
        std::vector<unsigned int> shell;
        calculate_shell(handle->voxel.bvalues,shell);
        ui->half_sphere->setChecked(is_dsi_half_sphere(shell));
        ui->scheme_balance->setChecked(is_multishell(shell) && handle->voxel.bvalues.size()-shell.back() < 100);
        if(is_dsi(shell))
        {
            ui->scheme_balance->setEnabled(false);
        }
        else
        // not dsi
        {
            if(ui->DSI->isChecked())
            {
                ui->GQI->setChecked(true);
                on_GQI_toggled(true);
            }
            ui->DSI->setEnabled(false);
            ui->half_sphere->setEnabled(false);
        }
        if(is_dsi(shell) || is_multishell(shell))
        {
            if(ui->QBI->isChecked())
            {
                ui->GQI->setChecked(true);
                on_GQI_toggled(true);
            }
            ui->QBI->setEnabled(false);
        }
    }
    if(!handle->is_human_data())
    {
        ui->csf_calibration->setEnabled(false);
        ui->csf_calibration->setVisible(false);
    }


}
void reconstruction_window::update_dimension(void)
{
    ui->SlicePos->setRange(0,handle->voxel.dim[2]-1);
    ui->SlicePos->setValue((handle->voxel.dim[2]-1) >> 1);
    ui->z_pos->setRange(0,handle->voxel.dim[2]-1);
    ui->z_pos->setValue((handle->voxel.dim[2]-1) >> 1);
    ui->x->setMaximum(handle->voxel.dim[0]-1);
    ui->y->setMaximum(handle->voxel.dim[1]-1);
    ui->z->setMaximum(handle->voxel.dim[2]-1);
    source_ratio = std::max(1.0,500/(double)handle->voxel.dim.height());
}

void reconstruction_window::load_b_table(void)
{
    ui->b_table->clear();
    ui->b_table->setRowCount(handle->voxel.bvalues.size());
    for(unsigned int index = 0;index < handle->voxel.bvalues.size();++index)
    {
        ui->b_table->setItem(index,0, new QTableWidgetItem(QString::number(handle->voxel.bvalues[index])));
        ui->b_table->setItem(index,1, new QTableWidgetItem(QString::number(handle->voxel.bvectors[index][0])));
        ui->b_table->setItem(index,2, new QTableWidgetItem(QString::number(handle->voxel.bvectors[index][1])));
        ui->b_table->setItem(index,3, new QTableWidgetItem(QString::number(handle->voxel.bvectors[index][2])));
    }
    ui->b_table->selectRow(0);
}
void reconstruction_window::on_b_table_itemSelectionChanged()
{
    v2c.set_range(ui->min_value->value(),ui->max_value->value());
    image::basic_image<float,2> tmp(image::geometry<2>(handle->voxel.dim[0],handle->voxel.dim[1]));
    unsigned int b_index = ui->b_table->currentRow();
    std::copy(handle->dwi_data[b_index] + ui->z_pos->value()*tmp.size(),
              handle->dwi_data[b_index] + ui->z_pos->value()*tmp.size() + tmp.size(),tmp.begin());
    buffer_source.resize(tmp.geometry());
    for(int i = 0;i < tmp.size();++i)
        buffer_source[i] = v2c[tmp[i]];
    source_image = QImage((unsigned char*)&*buffer_source.begin(),tmp.width(),tmp.height(),QImage::Format_RGB32).
                    scaled(tmp.width()*source_ratio,tmp.height()*source_ratio);
    show_view(source,source_image);
}


void reconstruction_window::resizeEvent ( QResizeEvent * event )
{
    QMainWindow::resizeEvent(event);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::showEvent ( QShowEvent * event )
{
    QMainWindow::showEvent(event);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::closeEvent(QCloseEvent *event)
{
    QMainWindow::closeEvent(event);

}

reconstruction_window::~reconstruction_window()
{
    delete ui;
}

void reconstruction_window::doReconstruction(unsigned char method_id,bool prompt)
{
    if(!handle.get())
        return;

    if (*std::max_element(handle->mask.begin(),handle->mask.end()) == 0)
    {
        QMessageBox::information(this,"error","Please select mask for reconstruction",0);
        return;
    }

    if (ui->odf_sharpening->currentIndex() == 1 && method_id != 1) // deconvolution
    {
        params[2] = ui->decon_param->value();
        settings.setValue("rec_deconvolution_param",params[2]);
    }
    if (ui->odf_sharpening->currentIndex() == 2 && method_id != 1) // decomposition
    {
        params[3] = ui->decom_fraction->value();
        params[4] = ui->decom_m->value();
        settings.setValue("rec_decomposition_param",params[3]);
        settings.setValue("rec_decom_m",params[4]);
    }
    //T1W DMDM
    if(method_id == 7 && ui->reg_method->currentIndex() == 4)
    {
        QString t1w_file_name1 =
                QFileInfo(handle->file_name.c_str()).absolutePath() + "/" + QFileInfo(handle->file_name.c_str()).baseName() + "_t1w.nii.gz";
        QString t1w_file_name2 =
                QFileInfo(handle->file_name.c_str()).absolutePath() + "/" + QFileInfo(handle->file_name.c_str()).baseName() + "_MPRAGE.nii.gz";
        if(QFileInfo(t1w_file_name1).exists())
            handle->voxel.t1w_file_name = t1w_file_name1.toStdString();
        else
        if(QFileInfo(t1w_file_name2).exists())
            handle->voxel.t1w_file_name = t1w_file_name2.toStdString();
        else
        {
            QMessageBox::information(0,"Reconstruction","Please Assign T1W file for normalization",0);
            QString filename = QFileDialog::getOpenFileName(
                    this,"Open T1W files",absolute_path,
                    "Images (*.nii *nii.gz);;All files (*)" );
            if( filename.isEmpty())
                return;
            handle->voxel.t1w_file_name = filename.toStdString();
        }
     }

    settings.setValue("rec_method_id",method_id);
    settings.setValue("rec_thread_num",ui->ThreadCount->value());
    settings.setValue("rec_odf_sharpening",ui->odf_sharpening->currentIndex());
    settings.setValue("rec_num_fiber",ui->NumOfFibers->value());
    settings.setValue("rec_gqi_def",ui->ODFDef->currentIndex());
    settings.setValue("rec_reg_method",ui->reg_method->currentIndex());
    settings.setValue("csf_calibration",ui->csf_calibration->isChecked() ? 1 : 0);


    settings.setValue("odf_order",ui->ODFDim->currentIndex());
    settings.setValue("rec_record_odf",ui->RecordODF->isChecked() ? 1 : 0);
    settings.setValue("output_jacobian",ui->output_jacobian->isChecked() ? 1 : 0);
    settings.setValue("output_mapping",ui->output_mapping->isChecked() ? 1 : 0);
    settings.setValue("output_diffusivity",ui->output_diffusivity->isChecked() ? 1 : 0);
    settings.setValue("output_tensor",ui->output_tensor->isChecked() ? 1 : 0);
    settings.setValue("output_rdi",(ui->rdi->isChecked() && method_id == 4) ? 1 : 0); // only for GQI
    settings.setValue("check_btable",ui->check_btable->isChecked() ? 1 : 0);

    begin_prog("reconstruction",true);
    int odf_order[8] = {4, 5, 6, 8, 10, 12, 16, 20};
    handle->voxel.ti.init(odf_order[ui->ODFDim->currentIndex()]);
    handle->voxel.odf_deconvolusion = 0;//ui->odf_sharpening->currentIndex() == 1 ? 1 : 0;
    handle->voxel.odf_decomposition = 0;//ui->odf_sharpening->currentIndex() == 2 ? 1 : 0;
    handle->voxel.odf_xyz[0] = ui->x->value();
    handle->voxel.odf_xyz[1] = ui->y->value();
    handle->voxel.odf_xyz[2] = ui->z->value();
    handle->voxel.csf_calibration = (ui->csf_calibration->isVisible() && ui->csf_calibration->isChecked()) ? 1: 0;
    handle->voxel.max_fiber_number = ui->NumOfFibers->value();
    handle->voxel.r2_weighted = ui->ODFDef->currentIndex();
    handle->voxel.reg_method = ui->reg_method->currentIndex();
    handle->voxel.need_odf = ui->RecordODF->isChecked() ? 1 : 0;
    handle->voxel.output_jacobian = ui->output_jacobian->isChecked() ? 1 : 0;
    handle->voxel.output_mapping = ui->output_mapping->isChecked() ? 1 : 0;
    handle->voxel.output_diffusivity = ui->output_diffusivity->isChecked() ? 1 : 0;
    handle->voxel.output_tensor = ui->output_tensor->isChecked() ? 1 : 0;
    handle->voxel.output_rdi = ui->rdi->isChecked() ? 1 : 0;
    handle->voxel.thread_count = ui->ThreadCount->value();

    if(method_id == 7 || method_id == 4)
    {
        handle->voxel.half_sphere = ui->half_sphere->isChecked() ? 1:0;
        handle->voxel.scheme_balance = ui->scheme_balance->isChecked() ? 1:0;
    }
    else
    {
        handle->voxel.half_sphere = false;
        handle->voxel.scheme_balance = false;
    }

    const char* msg = (const char*)reconstruction(handle.get(), method_id,
                                                  params,ui->check_btable->isChecked());
    if (!QFileInfo(msg).exists())
    {
        QMessageBox::information(this,"error",msg,0);
        return;
    }
    if(!prompt)
        return;

    QMessageBox::information(this,"DSI Studio","FIB file created.",0);
    if(method_id == 6)
        ((MainWindow*)parent())->addSrc(msg);
    else
        ((MainWindow*)parent())->addFib(msg);
}


void reconstruction_window::on_erosion_clicked()
{
    image::morphology::erosion(handle->mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_dilation_clicked()
{
    image::morphology::dilation(handle->mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_defragment_clicked()
{
    image::morphology::defragment(handle->mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_smoothing_clicked()
{
    image::morphology::smoothing(handle->mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::on_negate_clicked()
{
    image::morphology::negate(handle->mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_thresholding_clicked()
{
    bool ok;
    int threshold = QInputDialog::getInt(this,"DSI Studio","Please assign the threshold",
                                         (int)image::segmentation::otsu_threshold(dwi),
                                         (int)*std::min_element(dwi.begin(),dwi.end()),
                                         (int)*std::max_element(dwi.begin(),dwi.end())+1,1,&ok);
    if (!ok)
        return;
    image::threshold(dwi,handle->mask,threshold);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_load_mask_clicked()
{
    QString filename = QFileDialog::getOpenFileName(
            this,
            "Open region",
            absolute_path,
            "Mask files (*.txt *.nii *nii.gz *.hdr);;All files (*)" );
    if(filename.isEmpty())
        return;
    ROIRegion region(dwi.geometry(),handle->voxel.vs);
    std::vector<float> trans;
    region.LoadFromFile(filename.toLocal8Bit().begin(),trans);
    region.SaveToBuffer(handle->mask);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_save_mask_clicked()
{
    QString filename = QFileDialog::getSaveFileName(
            this,
            "Save region",
            absolute_path+"/mask.txt",
            "Text files (*.txt);;Nifti file(*nii.gz);;All files (*)" );
    if(filename.isEmpty())
        return;
    if(QFileInfo(filename.toLower()).completeSuffix() != "txt")
        filename = QFileInfo(filename).absolutePath() + "/" + QFileInfo(filename).baseName() + ".nii.gz";
    ROIRegion region(dwi.geometry(),handle->voxel.vs);
    region.LoadFromBuffer(handle->mask);
    std::vector<float> trans;
    region.SaveToFile(filename.toLocal8Bit().begin(),trans);
}

void reconstruction_window::on_doDTI_clicked()
{
    for(int index = 0;index < filenames.size();++index)
    {
        if(index)
        {
            begin_prog("load src");
            if(!load_src(index))
                return;
        }
        std::fill(params,params+5,0.0);
        if(ui->DTI->isChecked())
            doReconstruction(1,index+1 == filenames.size());
        else
        if(ui->DSI->isChecked())
        {
            params[0] = ui->hamming_filter->value();
            settings.setValue("rec_hamming_filter",params[0]);
            doReconstruction(0,index+1 == filenames.size());
        }
        else
        if(ui->QBI->isChecked())
        {
            params[0] = ui->regularization_param->value();
            params[1] = ui->SHOrder->value();
            settings.setValue("rec_qbi_reg",params[0]);
            settings.setValue("rec_qbi_sh_order",params[1]);
            doReconstruction(3,index+1 == filenames.size());
        }
        else
        if(ui->GQI->isChecked() || ui->QDif->isChecked())
        {
            params[0] = ui->diffusion_sampling->value();
            if(params[0] == 0.0)
                params[1] = ui->diffusion_time->value();
            settings.setValue("rec_gqi_sampling",ui->diffusion_sampling->value());
            if(ui->QDif->isChecked())
            {
                float res[4] = {0.5,1.0,2.0,3.0};
                params[1] = res[ui->resolution->currentIndex()];
                settings.setValue("rec_resolution",ui->resolution->currentIndex());
                doReconstruction(7,index+1 == filenames.size());
            }
            else
                doReconstruction(4,index+1 == filenames.size());
        }
        if(prog_aborted())
            break;
    }
}

void reconstruction_window::on_DTI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    //ui->ODFSharpening->setVisible(!checked);
    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(!checked);
    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_diffusivity->setVisible(checked);
    ui->output_tensor->setVisible(checked);
    ui->RecordODF->setVisible(!checked);
    ui->rdi->setVisible(!checked);



}

void reconstruction_window::on_DSI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    //ui->ODFSharpening->setVisible(checked);
    ui->DSIOption_2->setVisible(checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_diffusivity->setVisible(!checked);
    ui->output_tensor->setVisible(!checked);

    ui->RecordODF->setVisible(checked);
    ui->rdi->setVisible(!checked);

}

void reconstruction_window::on_QBI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    //ui->ODFSharpening->setVisible(checked);
    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(checked);
    ui->GQIOption_2->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_diffusivity->setVisible(!checked);
    ui->output_tensor->setVisible(!checked);
    ui->RecordODF->setVisible(checked);
    ui->rdi->setVisible(!checked);

}

void reconstruction_window::on_GQI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    //ui->ODFSharpening->setVisible(checked);
    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(!checked);
    ui->output_jacobian->setVisible(!checked);
    ui->output_diffusivity->setVisible(!checked);
    ui->output_tensor->setVisible(!checked);
    ui->RecordODF->setVisible(checked);

    ui->rdi->setVisible(checked);
    if(checked)
        ui->rdi->setChecked(true);
    if(ui->csf_calibration->isEnabled())
        ui->csf_calibration->setVisible(!ui->QDif->isChecked());
}

void reconstruction_window::on_QDif_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(checked);
    //ui->ODFSharpening->setVisible(checked);
    ui->DSIOption_2->setVisible(!checked);
    ui->QBIOption_2->setVisible(!checked);
    ui->GQIOption_2->setVisible(checked);

    ui->AdvancedOptions->setVisible(checked);
    ui->ODFOption->setVisible(checked);

    ui->output_mapping->setVisible(checked);
    ui->output_jacobian->setVisible(checked);
    ui->output_diffusivity->setVisible(!checked);
    ui->output_tensor->setVisible(!checked);
    ui->RecordODF->setVisible(checked);
    ui->rdi->setVisible(checked);
    if(checked)
        ui->rdi->setChecked(true);

}


void reconstruction_window::on_remove_background_clicked()
{
    for(int index = 0;index < handle->mask.size();++index)
        if(handle->mask[index] == 0)
            dwi[index] = 0;

    for(int index = 0;index < handle->dwi_data.size();++index)
    {
        unsigned short* buf = (unsigned short*)handle->dwi_data[index];
        for(int i = 0;i < handle->mask.size();++i)
            if(handle->mask[i] == 0)
                buf[i] = 0;
    }
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_zoom_in_clicked()
{
    source_ratio *= 1.1f;
    on_b_table_itemSelectionChanged();
}

void reconstruction_window::on_zoom_out_clicked()
{
    source_ratio *= 0.9f;
    on_b_table_itemSelectionChanged();
}

extern fa_template fa_template_imp;
void reconstruction_window::on_manual_reg_clicked()
{
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
            dwi,handle->voxel.vs,
            fa_template_imp.I,fa_template_imp.vs,
            image::reg::affine,image::reg::cost_type::corr));
    if(manual->exec() == QDialog::Accepted)
        handle->voxel.qsdr_trans = manual->T;
}

void reconstruction_window::on_odf_sharpening_currentIndexChanged(int)
{
    ui->xyz_widget->setVisible(ui->odf_sharpening->currentIndex() > 0);
    ui->decom_panel->setVisible(ui->odf_sharpening->currentIndex() == 2);
    ui->decon_param->setVisible(ui->odf_sharpening->currentIndex() == 1);
    on_RFSelection_currentIndexChanged(0);
}

void reconstruction_window::on_RFSelection_currentIndexChanged(int)
{
    ui->ODFSelection->setVisible(ui->RFSelection->currentIndex() > 0);
}

void reconstruction_window::on_AdvancedOptions_clicked()
{
    if(ui->AdvancedOptions->text() == "Advanced Options >>")
    {
        ui->AdvancedWidget->setVisible(true);
        ui->AdvancedOptions->setText("Advanced Options <<");
    }
    else
    {
        ui->AdvancedWidget->setVisible(false);
        ui->AdvancedOptions->setText("Advanced Options >>");
    }
}


void reconstruction_window::on_actionSave_4D_nifti_triggered()
{
    if(filenames.size() > 1)
    {
        for(int index = 0;check_prog(index,filenames.size());++index)
        {
            ImageModel model;
            if (!model.load_from_file(filenames[index].toLocal8Bit().begin()))
            {
                QMessageBox::information(this,"error",QString("Cannot open ") +
                    filenames[index] + " : " +handle->error_msg.c_str(),0);
                check_prog(0,0);
                return;
            }
            model.save_to_nii((filenames[index]+".nii.gz").toLocal8Bit().begin());
        }
        return;
    }
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save image as...",
                            filenames[0] + ".nii.gz",
                                "All files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_to_nii(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_b0_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save image as...",
                            filenames[0] + ".b0.nii.gz",
                                "All files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_b0_to_nii(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_b_table_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                            QFileInfo(filenames[0]).absolutePath() + "/b_table.txt",
                                "Text files (*.txt)" );
    if ( filename.isEmpty() )
        return;
    handle->save_b_table(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_bvals_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                                QFileInfo(filenames[0]).absolutePath() + "/bvals",
                                "Text files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_bval(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_bvecs_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                                QFileInfo(filenames[0]).absolutePath() + "/bvecs",
                                "Text files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_bvec(filename.toLocal8Bit().begin());
}

void reconstruction_window::update_image(void)
{
    dwi.resize(handle->voxel.dim);
    for(unsigned int index = 0;index < dwi.size();++index)
        dwi[index] = std::min<float>(254.0,handle->voxel.dwi_sum[index]*255.0);
    load_b_table();
}

void reconstruction_window::on_actionFlip_x_triggered()
{
    handle->flip(0);
    update_image();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_actionFlip_y_triggered()
{
    handle->flip(1);
    update_image();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_actionFlip_z_triggered()
{
    handle->flip(2);
    update_image();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_actionFlip_xy_triggered()
{
    begin_prog("rotating");
    handle->flip(3);
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::on_actionFlip_yz_triggered()
{
    begin_prog("rotating");
    handle->flip(4);
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::on_actionFlip_xz_triggered()
{
    begin_prog("rotating");
    handle->flip(5);
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}



bool load_image_from_files(QStringList filenames,image::basic_image<float,3>& ref,image::vector<3>& vs);
void reconstruction_window::on_actionRotate_triggered()
{
    QStringList filenames = QFileDialog::getOpenFileNames(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz *.dcm);;All files (*)" );
    if( filenames.isEmpty())
        return;

    image::basic_image<float,3> ref;
    image::vector<3> vs;
    if(!load_image_from_files(filenames,ref,vs))
        return;
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                                                dwi,handle->voxel.vs,ref,vs,
                                                                image::reg::rigid_body,
                                                                image::reg::cost_type::mutual_info));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    begin_prog("rotating");
    handle->rotate(ref.geometry(),manual->iT);
    handle->calculate_mask();
    handle->voxel.vs = vs;
    handle->voxel.report += " The diffusion images were rotated and scaled to the space of ";
    handle->voxel.report += filenames[0].toStdString();
    handle->voxel.report += ". The b-table was also rotated accordingly.";
    ui->report->setText(handle->voxel.report.c_str());
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());

}


void reconstruction_window::on_delete_2_clicked()
{
    if(handle->dwi_data.size() == 1)
        return;
    unsigned int index = ui->b_table->currentRow();
    ui->b_table->removeRow(index);
    handle->dwi_data.erase(handle->dwi_data.begin()+index);
    handle->voxel.bvalues.erase(handle->voxel.bvalues.begin()+index);
    handle->voxel.bvectors.erase(handle->voxel.bvectors.begin()+index);
}

void reconstruction_window::on_actionTrim_image_triggered()
{
    begin_prog("trimming");
    handle->trim();
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_diffusion_sampling_valueChanged(double arg1)
{
    if(arg1 == 0.0)
        ui->gqi_spectral->show();
    else
        ui->gqi_spectral->hide();
}

void reconstruction_window::on_SlicePos_valueChanged(int position)
{
    if (!dwi.size())
        return;
    buffer.resize(image::geometry<2>(dwi.width(),dwi.height()));
    unsigned int offset = position*buffer.size();
    std::copy(dwi.begin() + offset,dwi.begin()+ offset + buffer.size(),buffer.begin());

    unsigned char* slice_image_ptr = &*dwi.begin() + buffer.size()* position;
    unsigned char* slice_mask = &*handle->mask.begin() + buffer.size()* position;

    image::color_image buffer2(image::geometry<2>(dwi.width()*2,dwi.height()));
    image::draw(buffer,buffer2,image::vector<2,int>());
    for (unsigned int index = 0; index < buffer.size(); ++index)
    {
        unsigned char value = slice_image_ptr[index];
        if (slice_mask[index])
            buffer[index] = image::rgb_color(255, value, value);
        else
            buffer[index] = image::rgb_color(value, value, value);
    }
    image::draw(buffer,buffer2,image::vector<2,int>(dwi.width(),0));
    buffer2.swap(buffer);
    double ratio = std::max(1.0,
        std::min(((double)ui->graphicsView->width()-5)/(double)buffer.width(),
                 ((double)ui->graphicsView->height()-5)/(double)buffer.height()));
    slice_image = QImage((unsigned char*)&*buffer.begin(),buffer.width(),buffer.height(),QImage::Format_RGB32).
                    scaled(buffer.width()*ratio,buffer.height()*ratio);
    show_view(scene,slice_image);
}

void rec_motion_correction(ImageModel* handle)
{
    begin_prog("correcting");
    image::basic_image<float,3> I0;
    I0 = image::make_image(handle->dwi_data[0],handle->voxel.dim);
    image::mean(I0);
    image::filter::gradient_magnitude(I0);
    image::normalize(I0,1);
    image::par_for2(handle->voxel.bvalues.size(),[&](int i,int id)
    {
        if(i == 0 || prog_aborted())
            return;
        if(id == 0)
            check_prog(i*99/handle->voxel.bvalues.size(),100);
        image::basic_image<float,3> I1;
        I1 = image::make_image(handle->dwi_data[i],handle->voxel.dim);
        image::mean(I1);
        image::filter::gradient_magnitude(I1);
        image::normalize(I1,1);
        image::affine_transform<float> upper,lower;
        upper.translocation[0] = 2.0f;
        upper.translocation[1] = 2.0f;
        upper.translocation[2] = 2.0f;
        lower.translocation[0] = -2.0f;
        lower.translocation[1] = -2.0f;
        lower.translocation[2] = -2.0f;
        upper.rotation[0] = 3.1415926f*2.0f/180.0f;
        upper.rotation[1] = 3.1415926f*2.0f/180.0f;
        upper.rotation[2] = 3.1415926f*2.0f/180.0f;
        lower.rotation[0] = -3.1415926f*2.0f/180.0f;
        lower.rotation[1] = -3.1415926f*2.0f/180.0f;
        lower.rotation[2] = -3.1415926f*2.0f/180.0f;
        upper.scaling[0] = 1.02f;
        upper.scaling[1] = 1.02f;
        upper.scaling[2] = 1.02f;
        lower.scaling[0] = 0.98f;
        lower.scaling[1] = 0.98f;
        lower.scaling[2] = 0.98f;
        upper.affine[0] = 0.04f;
        upper.affine[1] = 0.04f;
        upper.affine[2] = 0.04f;
        lower.affine[0] = -0.04f;
        lower.affine[1] = -0.04f;
        lower.affine[2] = -0.04f;
        image::affine_transform<double> arg;
        image::reg::fun_adoptor<image::basic_image<float,3>,
                                image::vector<3>,
                                image::affine_transform<double>,
                                image::affine_transform<double>,
                                image::reg::mutual_information> fun(I0,handle->voxel.vs,I1,handle->voxel.vs,arg);
        bool terminated = false;
        double optimal_value = fun(arg);
        image::optimization::graient_descent(arg.begin(),arg.end(),
                                             upper.begin(),lower.begin(),fun,optimal_value,terminated,0.001);
        for(unsigned int i = 0;i < handle->voxel.bvalues.size();++i)
            handle->rotate_dwi(i,image::transformation_matrix<double>(arg,handle->voxel.dim,handle->voxel.vs,handle->voxel.dim,handle->voxel.vs));
    });
    check_prog(1,1);

}

void reconstruction_window::on_motion_correction_clicked()
{
    rec_motion_correction(handle.get());
    if(!prog_aborted())
    {
        handle->calculate_dwi_sum();
        handle->calculate_mask();
        update_image();
    }
}

void reconstruction_window::on_scheme_balance_toggled(bool checked)
{
    if(checked)
        ui->half_sphere->setChecked(false);
}



void reconstruction_window::on_half_sphere_toggled(bool checked)
{
    if(checked)
        ui->scheme_balance->setChecked(false);
}

bool add_other_image(ImageModel* handle,QString name,QString filename,bool full_auto)
{
    image::basic_image<float,3> ref;
    image::vector<3> vs;
    gz_nifti in;
    if(!in.load_from_file(filename.toLocal8Bit().begin()) || !in.toLPS(ref))
    {
        if(full_auto)
            std::cout << "Not a valid nifti file:" << filename.toStdString() << std::endl;
        else
            QMessageBox::information(0,"Error","Not a valid nifti file",0);
        return false;
    }
    image::transformation_matrix<double> affine;
    bool has_registered = false;
    for(unsigned int index = 0;index < handle->voxel.other_image.size();++index)
        if(ref.geometry() == handle->voxel.other_image[index].geometry())
        {
            affine = handle->voxel.other_image_affine[index];
            has_registered = true;
        }
    if(!has_registered && ref.geometry() != handle->voxel.dim)
    {
        in.get_voxel_size(vs.begin());
        if(full_auto)
        {
            std::cout << "add " << filename.toStdString() << " as " << name.toStdString() << std::endl;
            image::basic_image<float,3> from(handle->voxel.dwi_sum),to(ref);
            image::normalize(from,1.0);
            image::normalize(to,1.0);
            bool terminated = false;
            image::affine_transform<float> arg;
            image::reg::linear_mr(from,handle->voxel.vs,to,vs,arg,image::reg::rigid_body,image::reg::mutual_information(),terminated);
            image::reg::linear_mr(from,handle->voxel.vs,to,vs,arg,image::reg::rigid_body,image::reg::mutual_information(),terminated);
            affine = image::transformation_matrix<float>(arg,handle->voxel.dim,handle->voxel.vs,to.geometry(),vs);
        }
        else
        {
            std::shared_ptr<manual_alignment> manual(new manual_alignment(0,
                        handle->voxel.dwi_sum,handle->voxel.vs,ref,vs,image::reg::rigid_body,image::reg::cost_type::mutual_info));
            manual->on_rerun_clicked();
            if(manual->exec() != QDialog::Accepted)
                return false;
            affine = manual->T;
        }

    }
    handle->voxel.other_image.push_back(ref);
    handle->voxel.other_image_name.push_back(name.toLocal8Bit().begin());
    handle->voxel.other_image_affine.push_back(affine);
    return true;
}

void reconstruction_window::on_add_t1t2_clicked()
{
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz);;All files (*)" );
    if( filename.isEmpty())
        return;
    add_other_image(handle.get(),QFileInfo(filename).baseName(),filename,false);
}

void reconstruction_window::on_actionManual_Rotation_triggered()
{
    std::shared_ptr<manual_alignment> manual(
                new manual_alignment(this,dwi,handle->voxel.vs,dwi,handle->voxel.vs,image::reg::rigid_body,image::reg::cost_type::mutual_info));
    if(manual->exec() != QDialog::Accepted)
        return;
    begin_prog("rotating");
    handle->rotate(dwi.geometry(),manual->iT);
    handle->calculate_mask();
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}



void reconstruction_window::on_actionReplace_b0_by_T2W_image_triggered()
{
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz);;All files (*)" );
    if( filename.isEmpty())
        return;
    image::basic_image<float,3> ref;
    image::vector<3> vs;
    gz_nifti in;
    if(!in.load_from_file(filename.toLocal8Bit().begin()) || !in.toLPS(ref))
    {
        QMessageBox::information(this,"Error","Not a valid nifti file",0);
        return;
    }
    in.get_voxel_size(vs.begin());
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,dwi,handle->voxel.vs,ref,vs,image::reg::rigid_body,image::reg::cost_type::corr));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    begin_prog("rotating");
    handle->rotate(ref.geometry(),manual->iT);
    handle->calculate_mask();
    handle->voxel.vs = vs;
    image::pointer_image<unsigned short,3> I = image::make_image((unsigned short*)handle->dwi_data[0],handle->voxel.dim);
    ref *= (float)(*std::max_element(I.begin(),I.end()))/(*std::max_element(ref.begin(),ref.end()));
    std::copy(ref.begin(),ref.end(),I.begin());
    update_image();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_actionCorrect_AP_PA_scans_triggered()
{
    QMessageBox::information(this,"DSI Studio","Please assign another SRC file with phase encoding flipped",0);
    QString filename = QFileDialog::getOpenFileName(
            this,"Open SRC file",absolute_path,
            "Images (*.src.gz);;All files (*)" );
    if( filename.isEmpty())
        return;

    begin_prog("load src");
    ImageModel src2;
    if (!src2.load_from_file(filename.toLocal8Bit().begin()))
    {
        QMessageBox::information(this,"error",QString("Cannot open ") +
           filename + " : " +src2.error_msg.c_str(),0);
        check_prog(0,0);
        return;
    }
    check_prog(0,0);
    if(handle->voxel.dim != src2.voxel.dim)
    {
        QMessageBox::information(this,"error","The image dimension is different.",0);
        return;
    }

    image::basic_image<float,3> b01(handle->dwi_data[0],handle->voxel.dim),b02(src2.dwi_data[0],src2.voxel.dim),b0;
    b01 /= image::mean(b01);
    b02 /= image::mean(b02);
    b0 = b01;
    b0 += b02;
    b0 *= 0.5f;



}


