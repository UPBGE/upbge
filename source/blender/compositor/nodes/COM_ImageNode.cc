/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2011 Blender Foundation. */

#include "COM_ImageNode.h"
#include "COM_ConvertOperation.h"
#include "COM_MultilayerImageOperation.h"

#include "COM_SetColorOperation.h"
#include "COM_SetValueOperation.h"
#include "COM_SetVectorOperation.h"

namespace blender::compositor {

ImageNode::ImageNode(bNode *editor_node) : Node(editor_node)
{
  /* pass */
}
NodeOperation *ImageNode::do_multilayer_check(NodeConverter &converter,
                                              RenderLayer *render_layer,
                                              RenderPass *render_pass,
                                              Image *image,
                                              ImageUser *user,
                                              int framenumber,
                                              int outputsocket_index,
                                              int view,
                                              DataType datatype) const
{
  NodeOutput *output_socket = this->get_output_socket(outputsocket_index);
  MultilayerBaseOperation *operation = nullptr;
  switch (datatype) {
    case DataType::Value:
      operation = new MultilayerValueOperation(render_layer, render_pass, view);
      break;
    case DataType::Vector:
      operation = new MultilayerVectorOperation(render_layer, render_pass, view);
      break;
    case DataType::Color:
      operation = new MultilayerColorOperation(render_layer, render_pass, view);
      break;
    default:
      break;
  }
  operation->set_image(image);
  operation->set_image_user(user);
  operation->set_framenumber(framenumber);

  converter.add_operation(operation);
  converter.map_output_socket(output_socket, operation->get_output_socket());

  return operation;
}

void ImageNode::convert_to_operations(NodeConverter &converter,
                                      const CompositorContext &context) const
{
  /** Image output */
  NodeOutput *output_image = this->get_output_socket(0);
  bNode *editor_node = this->get_bnode();
  Image *image = (Image *)editor_node->id;
  ImageUser *imageuser = (ImageUser *)editor_node->storage;
  int framenumber = context.get_framenumber();
  bool output_straight_alpha = (editor_node->custom1 & CMP_NODE_IMAGE_USE_STRAIGHT_OUTPUT) != 0;
  BKE_image_user_frame_calc(image, imageuser, context.get_framenumber());
  /* force a load, we assume iuser index will be set OK anyway */
  if (image && image->type == IMA_TYPE_MULTILAYER) {
    bool is_multilayer_ok = false;
    ImBuf *ibuf = BKE_image_acquire_ibuf(image, imageuser, nullptr);
    if (image->rr) {
      RenderLayer *rl = (RenderLayer *)BLI_findlink(&image->rr->layers, imageuser->layer);
      if (rl) {
        is_multilayer_ok = true;

        for (int64_t index = 0; index < outputs_.size(); index++) {
          NodeOutput *socket = outputs_[index];
          NodeOperation *operation = nullptr;
          bNodeSocket *bnode_socket = socket->get_bnode_socket();
          NodeImageLayer *storage = (NodeImageLayer *)bnode_socket->storage;
          RenderPass *rpass = (RenderPass *)BLI_findstring(
              &rl->passes, storage->pass_name, offsetof(RenderPass, name));
          int view = 0;

          if (STREQ(storage->pass_name, RE_PASSNAME_COMBINED) &&
              STREQ(bnode_socket->name, "Alpha")) {
            /* Alpha output is already handled with the associated combined output. */
            continue;
          }

          /* returns the image view to use for the current active view */
          if (BLI_listbase_count_at_most(&image->rr->views, 2) > 1) {
            const int view_image = imageuser->view;
            const bool is_allview = (view_image == 0); /* if view selected == All (0) */

            if (is_allview) {
              /* heuristic to match image name with scene names
               * check if the view name exists in the image */
              view = BLI_findstringindex(
                  &image->rr->views, context.get_view_name(), offsetof(RenderView, name));
              if (view == -1) {
                view = 0;
              }
            }
            else {
              view = view_image - 1;
            }
          }

          if (rpass) {
            switch (rpass->channels) {
              case 1:
                operation = do_multilayer_check(converter,
                                                rl,
                                                rpass,
                                                image,
                                                imageuser,
                                                framenumber,
                                                index,
                                                view,
                                                DataType::Value);
                break;
                /* using image operations for both 3 and 4 channels (RGB and RGBA respectively) */
                /* XXX any way to detect actual vector images? */
              case 3:
                operation = do_multilayer_check(converter,
                                                rl,
                                                rpass,
                                                image,
                                                imageuser,
                                                framenumber,
                                                index,
                                                view,
                                                DataType::Vector);
                break;
              case 4:
                operation = do_multilayer_check(converter,
                                                rl,
                                                rpass,
                                                image,
                                                imageuser,
                                                framenumber,
                                                index,
                                                view,
                                                DataType::Color);
                break;
              default:
                /* dummy operation is added below */
                break;
            }
            if (index == 0 && operation) {
              converter.add_preview(operation->get_output_socket());
            }
            if (STREQ(rpass->name, RE_PASSNAME_COMBINED) && !(bnode_socket->flag & SOCK_UNAVAIL)) {
              for (NodeOutput *alpha_socket : get_output_sockets()) {
                bNodeSocket *bnode_alpha_socket = alpha_socket->get_bnode_socket();
                if (!STREQ(bnode_alpha_socket->name, "Alpha")) {
                  continue;
                }
                NodeImageLayer *alpha_storage = (NodeImageLayer *)bnode_socket->storage;
                if (!STREQ(alpha_storage->pass_name, RE_PASSNAME_COMBINED)) {
                  continue;
                }
                SeparateChannelOperation *separate_operation;
                separate_operation = new SeparateChannelOperation();
                separate_operation->set_channel(3);
                converter.add_operation(separate_operation);
                converter.add_link(operation->get_output_socket(),
                                   separate_operation->get_input_socket(0));
                converter.map_output_socket(alpha_socket, separate_operation->get_output_socket());
                break;
              }
            }
          }

          /* In case we can't load the layer. */
          if (operation == nullptr) {
            converter.set_invalid_output(get_output_socket(index));
          }
        }
      }
    }
    BKE_image_release_ibuf(image, ibuf, nullptr);

    /* without this, multilayer that fail to load will crash blender T32490. */
    if (is_multilayer_ok == false) {
      for (NodeOutput *output : get_output_sockets()) {
        converter.set_invalid_output(output);
      }
    }
  }
  else {
    const int64_t number_of_outputs = get_output_sockets().size();
    if (number_of_outputs > 0) {
      ImageOperation *operation = new ImageOperation();
      operation->set_image(image);
      operation->set_image_user(imageuser);
      operation->set_framenumber(framenumber);
      operation->set_render_data(context.get_render_data());
      operation->set_view_name(context.get_view_name());
      converter.add_operation(operation);

      if (output_straight_alpha) {
        NodeOperation *alpha_convert_operation = new ConvertPremulToStraightOperation();

        converter.add_operation(alpha_convert_operation);
        converter.map_output_socket(output_image, alpha_convert_operation->get_output_socket());
        converter.add_link(operation->get_output_socket(0),
                           alpha_convert_operation->get_input_socket(0));
      }
      else {
        converter.map_output_socket(output_image, operation->get_output_socket());
      }

      converter.add_preview(operation->get_output_socket());
    }

    if (number_of_outputs > 1) {
      NodeOutput *alpha_image = this->get_output_socket(1);
      ImageAlphaOperation *alpha_operation = new ImageAlphaOperation();
      alpha_operation->set_image(image);
      alpha_operation->set_image_user(imageuser);
      alpha_operation->set_framenumber(framenumber);
      alpha_operation->set_render_data(context.get_render_data());
      alpha_operation->set_view_name(context.get_view_name());
      converter.add_operation(alpha_operation);

      converter.map_output_socket(alpha_image, alpha_operation->get_output_socket());
    }
    if (number_of_outputs > 2) {
      NodeOutput *depth_image = this->get_output_socket(2);
      ImageDepthOperation *depth_operation = new ImageDepthOperation();
      depth_operation->set_image(image);
      depth_operation->set_image_user(imageuser);
      depth_operation->set_framenumber(framenumber);
      depth_operation->set_render_data(context.get_render_data());
      depth_operation->set_view_name(context.get_view_name());
      converter.add_operation(depth_operation);

      converter.map_output_socket(depth_image, depth_operation->get_output_socket());
    }
    if (number_of_outputs > 3) {
      /* happens when unlinking image datablock from multilayer node */
      for (int i = 3; i < number_of_outputs; i++) {
        NodeOutput *output = this->get_output_socket(i);
        NodeOperation *operation = nullptr;
        switch (output->get_data_type()) {
          case DataType::Value: {
            SetValueOperation *valueoperation = new SetValueOperation();
            valueoperation->set_value(0.0f);
            operation = valueoperation;
            break;
          }
          case DataType::Vector: {
            SetVectorOperation *vectoroperation = new SetVectorOperation();
            vectoroperation->setX(0.0f);
            vectoroperation->setY(0.0f);
            vectoroperation->setW(0.0f);
            operation = vectoroperation;
            break;
          }
          case DataType::Color: {
            SetColorOperation *coloroperation = new SetColorOperation();
            coloroperation->set_channel1(0.0f);
            coloroperation->set_channel2(0.0f);
            coloroperation->set_channel3(0.0f);
            coloroperation->set_channel4(0.0f);
            operation = coloroperation;
            break;
          }
        }

        if (operation) {
          /* not supporting multiview for this generic case */
          converter.add_operation(operation);
          converter.map_output_socket(output, operation->get_output_socket());
        }
      }
    }
  }
}  // namespace blender::compositor

}  // namespace blender::compositor
